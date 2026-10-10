/* uvcshow: live MJPEG capture from /dev/video0 -> libjpeg (DCT-scaled) -> /dev/fb0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <setjmp.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <poll.h>
#include <jpeglib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <linux/fb.h>

#define NBUF 2

static volatile int running = 1;
static void on_sig(int s) { (void)s; running = 0; }

typedef struct {
	void *start;
	size_t len;
} buf_t;

typedef struct {
	struct jpeg_error_mgr pub;
	jmp_buf jb;
} jerr_t;

static void jerr_exit(j_common_ptr c)
{
	jerr_t *e = (jerr_t *)c->err;
	longjmp(e->jb, 1);
}

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static unsigned char *decode_mem(const unsigned char *buf, unsigned len, int scale,
                                 int *ow, int *oh)
{
	struct jpeg_decompress_struct cinfo;
	jerr_t jerr;
	unsigned char *rgb = NULL, *row = NULL;
	int w, h, y;

	cinfo.err = jpeg_std_error(&jerr.pub);
	jerr.pub.error_exit = jerr_exit;
	if (setjmp(jerr.jb)) {
		jpeg_destroy_decompress(&cinfo);
		free(rgb); free(row);
		return NULL;
	}
	jpeg_create_decompress(&cinfo);
	jpeg_mem_src(&cinfo, buf, len);
	cinfo.do_fancy_upsampling = FALSE;
	cinfo.out_color_space = JCS_RGB;
	jpeg_read_header(&cinfo, TRUE);
	cinfo.scale_num = 1;
	cinfo.scale_denom = scale;
	jpeg_start_decompress(&cinfo);
	w = cinfo.output_width;
	h = cinfo.output_height;
	if (cinfo.output_components != 3) goto fail;
	rgb = malloc((size_t)w * h * 3);
	row = malloc((size_t)w * 3);
	if (!rgb || !row) goto fail;
	for (y = 0; y < h; y++) {
		JSAMPROW rp = row;
		jpeg_read_scanlines(&cinfo, &rp, 1);
		memcpy(rgb + (size_t)y * w * 3, row, (size_t)w * 3);
	}
	jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);
	free(row);
	*ow = w; *oh = h;
	return rgb;
fail:
	jpeg_destroy_decompress(&cinfo);
	free(rgb); free(row);
	return NULL;
}

static void fb_blit(unsigned char *rgb, int w, int h, int rot,
                    struct fb_var_screeninfo *var, struct fb_fix_screeninfo *fix, void *fb)
{
	int W, H, x, y, r = rot & 3, f, sw, sh, xoff, yoff;
	unsigned roff = var->red.offset, goff = var->green.offset, boff = var->blue.offset;
	unsigned char *dst;

	if (r == 1 || r == 3) { W = h; H = w; } else { W = w; H = h; }
	dst = malloc((size_t)W * H * 3);
	if (!dst) return;
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			int dx, dy;
			const unsigned char *s = rgb + ((size_t)y * w + x) * 3;
			unsigned char *d;
			switch (r) {
			case 1:  dx = h - 1 - y; dy = x; break;
			case 2:  dx = w - 1 - x; dy = h - 1 - y; break;
			case 3:  dx = y; dy = w - 1 - x; break;
			default: dx = x; dy = y; break;
			}
			d = dst + ((size_t)dy * W + dx) * 3;
			d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
		}
	}
	f = (int)((long)var->xres * 1000 / W);
	{
		int fy = (int)((long)var->yres * 1000 / H);
		if (fy < f) f = fy;
	}
	if (f < 10) f = 10;
	sw = (int)((long)W * f / 1000);
	sh = (int)((long)H * f / 1000);
	if (sw > (int)var->xres) sw = var->xres;
	if (sh > (int)var->yres) sh = var->yres;
	xoff = (var->xres - sw) / 2;
	yoff = (var->yres - sh) / 2;
	{
		unsigned long sx_step = ((unsigned long)W << 16) / (unsigned long)sw;
		unsigned long sy_step = ((unsigned long)H << 16) / (unsigned long)sh;
		unsigned long fx;
		int j;
		for (y = 0; y < yoff; y++) {
			uint32_t *line = (uint32_t *)((char *)fb + (size_t)y * fix->line_length);
			for (x = 0; x < (int)var->xres; x++) line[x] = 0;
		}
		for (j = 0; j < sh; j++) {
			const unsigned char *srow = dst + (size_t)((sy_step * (unsigned long)j) >> 16) * W * 3;
			uint32_t *line = (uint32_t *)((char *)fb + (size_t)(yoff + j) * fix->line_length);
			for (x = 0; x < xoff; x++) line[x] = 0;
			for (fx = 0, x = 0; x < sw; x++) {
				const unsigned char *s = srow + (size_t)(fx >> 16) * 3;
				line[xoff + x] = ((uint32_t)s[0] << roff) |
				                 ((uint32_t)s[1] << goff) |
				                 ((uint32_t)s[2] << boff);
				fx += sx_step;
			}
			for (x = xoff + sw; x < (int)var->xres; x++) line[x] = 0;
		}
		for (y = yoff + sh; y < (int)var->yres; y++) {
			uint32_t *line = (uint32_t *)((char *)fb + (size_t)y * fix->line_length);
			for (x = 0; x < (int)var->xres; x++) line[x] = 0;
		}
	}
	free(dst);
}

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
	return r;
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/video0";
	int rot = 1, scale = 2, maxframes = 0, skip = 3, fd, fd_fb, i;
	int to_ms = 6000, rc = 0;
	struct v4l2_format fmt;
	struct v4l2_requestbuffers req;
	struct v4l2_buffer buf;
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	buf_t bufs[NBUF];
	void *fb;
	size_t fblen;
	int n_shown = 0, n_bad = 0, n_dec_fail = 0, n_pipe = 0, n_seen = 0;
	int last_line_len;
	double t_start, t_report, t_prev_report;
	int n_at_prev = 0;
	long dec_ms = 0, blit_ms = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-r") && i + 1 < argc) rot = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-s") && i + 1 < argc) scale = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-n") && i + 1 < argc) maxframes = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-k") && i + 1 < argc) skip = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-d") && i + 1 < argc) dev = argv[++i];
		else if (!strcmp(argv[i], "-t") && i + 1 < argc) to_ms = atoi(argv[++i]);
	}
	if (scale != 1 && scale != 2 && scale != 4 && scale != 8) scale = 2;

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

	memset(bufs, 0, sizeof(bufs));
	fd = open(dev, O_RDWR);
	if (fd < 0) { perror(dev); return 1; }

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = 240;
	fmt.fmt.pix.height = 320;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) { perror("S_FMT"); return 1; }
	printf("negotiated: %ux%u fourcc=%.4s sizeimage=%u\n",
	       fmt.fmt.pix.width, fmt.fmt.pix.height,
	       (char *)&fmt.fmt.pix.pixelformat, fmt.fmt.pix.sizeimage);

	memset(&req, 0, sizeof(req));
	req.count = NBUF;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) { perror("REQBUFS"); return 1; }
	if ((int)req.count < NBUF) { fprintf(stderr, "only %u buffers\n", req.count); return 1; }

	for (i = 0; i < NBUF; i++) {
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) { perror("QUERYBUF"); return 1; }
		bufs[i].len = buf.length;
		bufs[i].start = mmap(NULL, bufs[i].len, PROT_READ | PROT_WRITE,
		                     MAP_SHARED, fd, buf.m.offset);
		if (bufs[i].start == MAP_FAILED) { perror("mmap buf"); return 1; }
	}
	for (i = 0; i < NBUF; i++) {
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) { perror("QBUF"); return 1; }
	}
	{
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) { perror("STREAMON"); return 1; }
	}

	fd_fb = open("/dev/fb0", O_RDWR);
	if (fd_fb < 0) { perror("/dev/fb0"); return 1; }
	if (xioctl(fd_fb, FBIOGET_VSCREENINFO, &var) || xioctl(fd_fb, FBIOGET_FSCREENINFO, &fix)) {
		perror("fb ioctl"); return 1;
	}
	last_line_len = fix.line_length;
	fblen = fix.line_length * (size_t)var.yres_virtual;
	fb = mmap(NULL, fblen, PROT_READ | PROT_WRITE, MAP_SHARED, fd_fb, 0);
	if (fb == MAP_FAILED) { perror("mmap fb"); return 1; }

	printf("showing %s rot=%d scale=1/%d skip=%d\n", dev, rot, scale, skip);
	fflush(stdout);
	t_start = now_s();
	t_report = t_start;
	t_prev_report = t_start;

	while (running) {
		unsigned char *rgb;
		int w = 0, h = 0;
		unsigned len;
		double t0, t1, t2;
		struct pollfd pf;
		int pr;

		pf.fd = fd;
		pf.events = POLLIN;
		pf.revents = 0;
		pr = poll(&pf, 1, to_ms);
		if (pr < 0) { if (errno == EINTR) continue; perror("poll"); rc = 1; break; }
		if (pr == 0) {
			fprintf(stderr, "TIMEOUT: no frame for %d ms (shown=%d bad=%d decfail=%d pipe=%d) - USB link? check musb errors\n",
			        to_ms, n_shown, n_bad, n_dec_fail, n_pipe);
			rc = 3;
			break;
		}

		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		if (xioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
			if (errno == EINTR) continue;
			if (errno == EPIPE) {
				n_pipe++;
				fprintf(stderr, "stream overflow (#%d), requeue\n", n_pipe);
				memset(&buf, 0, sizeof(buf));
				buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
				buf.memory = V4L2_MEMORY_MMAP;
				buf.index = 0;
				if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) { perror("QBUF after pipe"); rc = 1; break; }
				buf.index = 1;
				if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) { perror("QBUF after pipe"); rc = 1; break; }
				continue;
			}
			perror("DQBUF");
			rc = 1;
			break;
		}
		t0 = now_s();
		n_seen++;
		if (t0 - t_report >= 2.0) {
			double win = t0 - t_prev_report;
			int dn = n_shown - n_at_prev;
			printf("seen=%d shown=%d fps=%.1f bad=%d decfail=%d pipe=%d\n",
			       n_seen, n_shown, win > 0 ? dn / win : 0, n_bad, n_dec_fail, n_pipe);
			fflush(stdout);
			n_at_prev = n_shown; t_prev_report = t0; t_report = t0;
		}
		len = buf.bytesused;
		if (len > bufs[buf.index].len) len = bufs[buf.index].len;

		if (n_seen <= skip || len < 4 ||
		    ((unsigned char *)bufs[buf.index].start)[0] != 0xff ||
		    ((unsigned char *)bufs[buf.index].start)[1] != 0xd8) {
			if (n_seen > skip) n_bad++;
			if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) { rc = 1; break; }
			continue;
		}

		rgb = decode_mem(bufs[buf.index].start, len, scale, &w, &h);
		if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) { perror("QBUF back"); break; }
		t1 = now_s();
		if (!rgb) {
			n_dec_fail++;
			continue;
		}
		fb_blit(rgb, w, h, rot, &var, &fix, fb);
		free(rgb);
		t2 = now_s();
		n_shown++;
		dec_ms += (long)((t1 - t0) * 1000);
		blit_ms += (long)((t2 - t1) * 1000);

		if (n_shown == 1)
			printf("first: %dx%d decode %dms blit %dms\n", w, h,
			       (int)((t1 - t0) * 1000), (int)((t2 - t1) * 1000));

		if (maxframes && n_shown >= maxframes) break;
	}
	{
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		if (xioctl(fd, VIDIOC_STREAMOFF, &type) < 0) { perror("STREAMOFF"); }
	}
	for (i = 0; i < NBUF; i++)
		if (bufs[i].start && bufs[i].start != MAP_FAILED)
			munmap(bufs[i].start, bufs[i].len);
	printf("exit: seen=%d shown=%d bad=%d decfail=%d pipe=%d ll=%d\n", n_seen, n_shown, n_bad, n_dec_fail, n_pipe, last_line_len);
	(void)t_start;
	return rc;
}
