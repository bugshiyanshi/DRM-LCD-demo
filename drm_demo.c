#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <assert.h>
#include <errno.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

struct kms_config {
	uint32_t fb_width;	//缓冲区对象的宽度
	uint32_t fb_height;	//缓冲区对象的高度
	uint32_t fb_line_size;	//缓冲区对象每行像素数，单位字节
	uint32_t fb_size;	//缓冲区对象总的像素数，单位字节
	uint32_t fb_handle;	//缓冲区对象的引用句柄
	uint32_t *fb_map;	//指向缓冲区的内存映射地址

	uint32_t fb_id;		//扫描缓冲区的缓冲区对象ID
	uint32_t conn_id;	//与缓冲区绑定的连接器ID
	uint32_t crtc_id;	//与连接器绑定的CRTC ID
	drmModeCrtc *old_crtc;	//更改前CRTC配置，退出时恢复原模式
	drmModeModeInfo mode;	//使用的显示模式配置(时序有关)
};

struct kms_config kms;


int drm_check_capability(int fd)
{
	uint64_t flag;

	if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &flag) < 0 || !flag) {

		perror("this drm device does not support dumb buffer");
		return -1;

	}

	return 0;
	
}

int drm_open(int *pfd, const char *dev)
{
	int fd, err;

	assert(NULL != dev);

	fd = open(dev, O_RDWR);
	if (fd < 0) {

		printf("cannot open drm device '%s' \n", dev);
		return fd;
	}

	/* 1.2 检查DRM设备是否支持DRM_CAP_DUMB_BUFFER特性 */
	
	err = drm_check_capability(fd);
	if (err < 0) {
		close(fd);
		return err;
	}

	*pfd = fd;

	return 0;


}


void drm_kms_init(drmModeConnector *conn, int crtc_id)
{

	kms.crtc_id = crtc_id;
	memcpy(&kms.mode, &conn->modes[0], sizeof(kms.mode));
	kms.conn_id   = conn->connector_id;
	kms.fb_width  = conn->modes[0].hdisplay;
	kms.fb_height = conn->modes[0].vdisplay;

	printf("bind crtc:%d + encoder:%d + connector:%d \n", kms.crtc_id,
						conn->encoder_id, kms.conn_id);

}

int drm_find_crtc(int fd, drmModeRes *res, drmModeConnector *conn)
{

	drmModeEncoder *encoder;

	int i, j;

	for (i = 0; i < conn->count_encoders; i++) {


		encoder = drmModeGetEncoder(fd, conn->encoders[i]);
		if (!encoder)
			continue;

		for (j = 0; j < res->count_crtcs; j++) {

			/* 判断crtc是否与编码器适配 */
			if (!(encoder->possible_crtcs & (1 << j)))
				continue;

			drm_kms_init(conn, encoder->crtc_id);

			drmModeFreeEncoder(encoder);
			return 0;

		}

		drmModeFreeEncoder(encoder);
		encoder = NULL;

	}

	printf("cannot find suitable crtc for connector\n");
	
	return -ENOENT;

}

int drm_find_encoder(int fd, drmModeRes * res, drmModeConnector *conn)
{

	drmModeEncoder *encoder;

	/* 判断该连接器是否有绑定的编码器 */
	
	if (conn->encoder_id)
		encoder = drmModeGetEncoder(fd, conn->encoder_id);
	else 
		encoder = NULL;

	/* 判断该编码器是否有绑定的crtc */
	if (encoder) {

		if (encoder->crtc_id) {

			drm_kms_init(conn, encoder->crtc_id);
			drmModeFreeEncoder(encoder);
			return 0;
		}
	
		drmModeFreeEncoder(encoder);
		encoder = NULL;	

	}

	return drm_find_crtc(fd, res, conn);

}

int drm_find_connector(int fd, drmModeRes *res)
{
	drmModeConnector *conn;

	int i, err;

	for (i = 0; i < res->count_connectors; i++) {

		conn = drmModeGetConnector(fd, res->connectors[i]);
		if (!conn)
			continue;

		/* 判断连接器是否已连接，否则忽略
		 * 判断连接器是否有有效的模式，没有也忽略
		 */
		if ((DRM_MODE_CONNECTED != conn->connection) || (0 == conn->count_modes))
			continue;

		err = drm_find_encoder(fd, res, conn);	
		if (err < 0) {
		
			drmModeFreeConnector(conn);
			continue;
		}

		drmModeFreeConnector(conn);
		return 0;

	}
	
	drmModeFreeConnector(conn);

	return -1;
}



int drm_fb_mmap(int fd)
{

	struct drm_mode_map_dumb mreq;
	
	int err;

	memset(&mreq, 0, sizeof(mreq));

	mreq.handle = kms.fb_handle;

	err = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
	if (err < 0 ) {

		perror("MODE_MAP_DUMB err");
		return -errno;
	}

	kms.fb_map = mmap(0, kms.fb_size, PROT_READ | PROT_WRITE, MAP_SHARED,
				fd, mreq.offset);
	if (MAP_FAILED == kms.fb_map) {

		perror("cannot mmap dumb buffer ");
		return -errno;
	}

	memset(kms.fb_map, 0, kms.fb_size);

	return 0;
}

int drm_crtc_fb(int fd)
{

	struct drm_mode_create_dumb creq;
	struct drm_mode_destroy_dumb dreq;

	int err;

	memset(&creq, 0, sizeof(creq));
	creq.width  = kms.fb_width;
	creq.height = kms.fb_height;
	creq.bpp    = 32;

	err = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
	if (err < 0) {

		perror("create dumb buffer error ");
		return -errno;
	}

	kms.fb_line_size = creq.pitch;
	kms.fb_size = creq.size;
	kms.fb_handle = creq.handle;

	/* 创建帧缓存区对象 */
	
	err = drmModeAddFB(fd, kms.fb_width, kms.fb_height, 24, 32,
				kms.fb_line_size, kms.fb_handle, &kms.fb_id);

	if (err < 0) {

		perror("create framebuffer error ");
		goto out;
	}

	/* 内存映射 */
	
	err = drm_fb_mmap(fd);
	if (err < 0) 
		goto mmap_err;

	return 0;


mmap_err:
	drmModeRmFB(fd, kms.fb_id);

out:
	memset(&dreq, 0, sizeof(dreq));
	dreq.handle = kms.fb_handle;
	drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
	return -errno;

}


int drm_get_res(int fd)
{
	drmModeRes *res;

	int err;

	res = drmModeGetResources(fd);
	if (!res) {

		perror("cannot get drm resources ,err ");
		return -errno;
	}

	/* 2.2 查找适配的连接器 */

	err = drm_find_connector(fd, res);
	if (err < 0)
		goto out;
	
	/* 3. 为crtc申请扫描缓冲区 */
	err = drm_crtc_fb(fd);
	
	


out:
	drmModeFreeResources(res);
	return err;	

}


void drm_free_fb(int fd)
{
	struct drm_mode_destroy_dumb dreq;
	
	int err;

	/* 取消内存映射 */
	err = munmap(kms.fb_map, kms.fb_size);
	if (err < 0)
		perror("munmap fb errno ");

	/* 释放帧缓冲区对象*/
	drmModeRmFB(fd, kms.fb_id);

	/* 销毁dumb缓冲区 */
	memset(&dreq, 0, sizeof(dreq));
	dreq.handle = kms.fb_handle;
	drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);

}

int drm_kms_set(int fd)
{
	int err;

	kms.old_crtc = drmModeGetCrtc(fd, kms.crtc_id);

	err = drmModeSetCrtc(fd, kms.crtc_id, kms.fb_id, 0, 0,
					&kms.conn_id, 1, &kms.mode);
	if (err < 0) {

		printf("cannot set CRTC for connector \n");

		return err;
	}

	printf("set crtc succeed\n");

	return 0;
}

void drm_draw_background(void)
{
	int i, k;

	int color[] = {0xff0000, 0xffff00, 0x00ff00}; //红、黄、绿

	for (k = 0; k < 3; k++) {

		for (i = 0; i < (kms.fb_size / 4); i++)
			kms.fb_map[i] = color[k];

		sleep(2);
	}

}

int main(void)
{
	int err, fd;

	
	/* 1.1 打开DRM设备 */
	const char *card = "/dev/dri/card0";

	err = drm_open(&fd, card);
	if (err < 0)
		goto out;

	
	/* 2. 查找适配的"crtc + encoder + connector" */
	
	err = drm_get_res(fd);
	if (err < 0)
		goto drm_err;

	/* 4.KMS模式设置 */
	err = drm_kms_set(fd);
	if (err < 0) 
		goto kms_err;


	/* 5 绘制背景 */
	
	drm_draw_background();



kms_err:
	drm_free_fb(fd);

drm_err:
	close(fd);

out:
	return err;
}
