#ifndef _SC132CAMERA_H
#define _SC132CAMERA_H

#include <stdint.h>

#include "hbn_api.h"

/*
定义回调数据
@y_plane / @uv_plane  Y / UV 平面虚拟地址（hbn_vnode DMA 缓冲）
@y_phys  / @uv_phys   Y / UV 平面物理地址（用于下游零拷贝送编码器）
@y_len / @uv_len      数据长度
@width  宽度
@height 高度
*/
typedef void (*sc132GetDataCallBack)(void *y_plane, void *uv_plane,
                                    uint64_t y_phys, uint64_t uv_phys,
                                    long y_len, long uv_len,
                                    int width, int height,
                                    uint64_t timestamp);
typedef struct sc132_frame sc132_frame_t;
typedef void (*sc132FrameCallBack)(sc132_frame_t *frame, void *user);

#define SC132_FRAME_SET_MAX_CAMERAS 4

typedef struct {
	sc132FrameCallBack ch0_cb;
	void *ch0_user;
	sc132FrameCallBack ch1_cb;
	void *ch1_user;
	sc132FrameCallBack ch2_cb;
	void *ch2_user;
	sc132FrameCallBack ch3_cb;
	void *ch3_user;
	int width;
	int height;
} sc132_frame_callbacks_t;

typedef struct {
	sc132_frame_t *frame;
	uint32_t camera_id;
	uint64_t sequence;
	uint32_t frame_id;
	uint64_t timestamp_ns;
	int width;
	int height;
} sc132_frame_set_item_t;

typedef struct {
	uint64_t group_id;
	uint32_t camera_count;
	uint64_t group_timestamp_ns;
	uint64_t max_skew_ns;
	sc132_frame_set_item_t items[SC132_FRAME_SET_MAX_CAMERAS];
} sc132_frame_set_t;

typedef void (*sc132FrameSetCallBack)(const sc132_frame_set_t *frame_set, void *user);

typedef struct {
	sc132FrameSetCallBack cb;
	void *user;
	int camera_count;
	uint64_t max_skew_ns;
	uint32_t timeout_ms;
	int width;
	int height;
} sc132_frame_set_config_t;

//typedef void (*sc132GetDataCallBack)(void *camera, long len, int width, int height, uint64_t time_stamp);
/*初始化HD
@HDGetData_dist 相机1-4 回调
@width 设置输出宽度	
@width 设置输出高度		
*/
int VioCamInitm(sc132GetDataCallBack sc132GetData_dist1,
				sc132GetDataCallBack sc132GetData_dist2,
				sc132GetDataCallBack sc132GetData_dist3,
				sc132GetDataCallBack sc132GetData_dist4,
				int width, int height);
int VioCamStartOnly(int camera_count, int width, int height);
int VioCamInitmFrame(const sc132_frame_callbacks_t *cfg);
int VioCamInitmFrameSet(const sc132_frame_set_config_t *cfg);
int VioCamInitmFrameSetMask(const sc132_frame_set_config_t *cfg, uint32_t camera_mask);
int VioCamSetFps(int fps);
int VioCamSetOutputRotate(int rotate_clockwise_degree);
int sc132_frame_retain(sc132_frame_t *frame);
void sc132_frame_release(sc132_frame_t *frame);
const hb_mem_graphic_buf_t *sc132_frame_get_graphic_buf(const sc132_frame_t *frame);
int sc132_frame_get_camera_id(const sc132_frame_t *frame);
uint64_t sc132_frame_get_sequence(const sc132_frame_t *frame);
uint64_t sc132_frame_get_timestamp_ns(const sc132_frame_t *frame);
uint32_t sc132_frame_get_frame_id(const sc132_frame_t *frame);
int sc132_frame_get_width(const sc132_frame_t *frame);
int sc132_frame_get_height(const sc132_frame_t *frame);

//关闭HD
void VioCamClose(void);
#endif
