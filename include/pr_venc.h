#ifndef _PR_RTSP_H
#define _PR_RTSP_H
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdint.h>
/* 初始化默认 RTSP 通道。
@width/@height: 输入 NV12 图像宽高。
@fps: 编码帧率。
@bps: 编码目标平均码率，单位 kbps；例如 2000 表示约 2Mbps。
说明：默认通道内部使用 context_ch1，RTSP 端口固定 554，URL 固定 /PRR。
*/
int init_rtsp(int width,int height,int fps,long long _bps);
/* 初始化默认 RTSP 通道，并让 VENC 在编码前按顺时针角度旋转输入帧。
@rotate_clockwise_degree: 仅支持 0/90/180/270；输入仍为 width/height，码流显示尺寸由 VENC 旋转后决定。
*/
int init_rtsp_rotate(int width,int height,int fps,long long _bps,int rotate_clockwise_degree);

/* 初始化连续编号 RTSP 扩展通道。
@width/@height: 输入 NV12 图像宽高。
@fps: 编码帧率。
@bps: 编码目标平均码率，单位 kbps；例如 2000 表示约 2Mbps。
@port: RTSP 端口；四目 demo 默认使用 554/555/556/557。
@URL: 流媒体路径，格式示例 /PRR。
说明：默认无编号通道保留给兼容入口；ch1-ch4 用于四目相机推流。
如需单独一路非四目示例推流，可使用默认无编号通道，并避免和 ch1 同时占用 554 端口。
*/
int init_rtsp_ch1(int width,int height,int fps,long long _bps,int port,char* URL);
int init_rtsp_ch1_rotate(int width,int height,int fps,long long _bps,int port,char* URL,
		int rotate_clockwise_degree);
int init_rtsp_ch2(int width,int height,int fps,long long _bps,int port,char* URL);
int init_rtsp_ch2_rotate(int width,int height,int fps,long long _bps,int port,char* URL,
		int rotate_clockwise_degree);
int init_rtsp_ch3(int width,int height,int fps,long long _bps,int port,char* URL);
int init_rtsp_ch3_rotate(int width,int height,int fps,long long _bps,int port,char* URL,
		int rotate_clockwise_degree);
int init_rtsp_ch4(int width,int height,int fps,long long _bps,int port,char* URL);
int init_rtsp_ch4_rotate(int width,int height,int fps,long long _bps,int port,char* URL,
		int rotate_clockwise_degree);

/*
@data: 单块连续 NV12 图像虚拟地址，布局为 Y 平面后接 interleaved UV 平面。
@phys: 同一块 DMA 缓冲区的 Y 平面物理地址，用于 X5 编码器零拷贝 DMA。
@len: 当前实现未使用，输入大小按初始化 width * height * 3 / 2 推导。
@time_stamp1: 相机/上层回调时间戳，单位 ns；内部会转换为 us 作为编码器 PTS。
*/
void Rtsp_SendImg(void * data,uint64_t phys,int len,uint64_t time_stamp1);
void Rtsp_SendImg_planes(void *y_data, void *uv_data,
		uint64_t y_phys, uint64_t uv_phys, int len, uint64_t time_stamp1);

/*
@data: 单块连续 NV12 图像虚拟地址，布局为 Y 平面后接 interleaved UV 平面。
@phys: 同一块 DMA 缓冲区的 Y 平面物理地址。
@len: 当前实现未使用，输入大小按初始化 width * height * 3 / 2 推导。
@time_stamp*: 相机/上层回调时间戳，单位 ns；内部会转换为 us。
说明：*_planes 变体用于 Y/UV 分离的虚拟地址和物理地址。
*/
void Rtsp_SendImg_ch1(void * data,uint64_t phys,int len,uint64_t time_stamp2);
void Rtsp_SendImg_ch1_planes(void *y_data, void *uv_data,
		uint64_t y_phys, uint64_t uv_phys, int len, uint64_t time_stamp2);
void Rtsp_SendImg_ch2(void * data,uint64_t phys,int len,uint64_t time_stamp3);
void Rtsp_SendImg_ch2_planes(void *y_data, void *uv_data,
		uint64_t y_phys, uint64_t uv_phys, int len, uint64_t time_stamp3);
void Rtsp_SendImg_ch3(void * data,uint64_t phys,int len,uint64_t time_stamp3);
void Rtsp_SendImg_ch3_planes(void *y_data, void *uv_data,
		uint64_t y_phys, uint64_t uv_phys, int len, uint64_t time_stamp3);
void Rtsp_SendImg_ch4(void * data,uint64_t phys,int len,uint64_t time_stamp4);
void Rtsp_SendImg_ch4_planes(void *y_data, void *uv_data,
		uint64_t y_phys, uint64_t uv_phys, int len, uint64_t time_stamp4);

//关闭HD
void rtspClose(void);

//关闭HD
void rtspClose_ch1(void);
void rtspClose_ch2(void);
void rtspClose_ch3(void);
void rtspClose_ch4(void);
#endif
