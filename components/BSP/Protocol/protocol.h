#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

// 帧头
#define FRAME_HEADER_1  0x53
#define FRAME_HEADER_2  0x59

// 控制字 (心率监测功能)
#define CTRL_HEART_RATE 0x85
// 控制字 (人体存在/运动信息)
#define CTRL_HUMAN_PRESENCE 0x80
// 控制字 (呼吸监测)
#define CTRL_BREATH     0x81
// 控制字 (睡眠监测)
#define CTRL_SLEEP      0x84

// 帧尾
#define FRAME_TAIL_1    0x54
#define FRAME_TAIL_2    0x43

// 最小帧长度 (Header(2) + Ctrl(1) + Cmd(1) + Len(2) + Checksum(1) + Tail(2))
#define MIN_FRAME_LEN   9

// 命令字 - 心率
#define CMD_HEART_RATE_SWITCH 0x00
#define CMD_HEART_RATE_REPORT 0x02

// 命令字 - 人体存在/运动 (CTRL_HUMAN_PRESENCE 0x80)
#define CMD_MOTION_INFO       0x02 // 运动信息 (静止/活跃)
#define CMD_BODY_MOVEMENT     0x03 // 体动参数
#define CMD_HUMAN_DISTANCE    0x04 // 人体距离
#define CMD_HUMAN_ORIENTATION 0x05 // 人体方位

// 命令字 - 呼吸 (CTRL_BREATH 0x81)
#define CMD_BREATH_VALUE      0x02 // 呼吸数值

// 命令字 - 睡眠 (CTRL_SLEEP 0x84)
#define CMD_SLEEP_COMPREHENSIVE 0x0C // 睡眠综合状态上报
#define CMD_SLEEP_QUALITY       0x0D // 睡眠质量分析上报

// 开关状态
#define HEART_RATE_ON  0x01
#define HEART_RATE_OFF 0x00

/**
 * @brief 构建协议帧
 * 
 * @param ctrl      控制字 (例如 CTRL_HEART_RATE)
 * @param cmd       命令字
 * @param data      数据指针 (可以为 NULL，如果 data_len 为 0)
 * @param data_len  数据长度
 * @param out_buf   输出缓冲区，用于存储构建好的帧
 * @param out_len   输入时为缓冲区大小，输出时为实际帧长度
 * @return int      0: 成功, -1: 缓冲区过小
 */
int protocol_build_frame(uint8_t ctrl, uint8_t cmd, const uint8_t *data, uint16_t data_len, uint8_t *out_buf, uint16_t *out_len);

/**
 * @brief 构建心率监测开关指令帧
 * 
 * @param enable    1: 开启, 0: 关闭
 * @param out_buf   输出缓冲区
 * @param out_len   输入时为缓冲区大小，输出时为实际帧长度
 * @return int      0: 成功, -1: 缓冲区过小
 */
int protocol_pack_heart_rate_switch(uint8_t enable, uint8_t *out_buf, uint16_t *out_len);

/**
 * @brief 解析协议帧
 * 
 * @param buffer        接收到的数据缓冲区
 * @param len           缓冲区长度
 * @param out_ctrl      输出: 控制字
 * @param out_cmd       输出: 命令字
 * @param out_data      输出: 数据指针 (指向 buffer 内部)
 * @param out_data_len  输出: 数据长度
 * @return int          0: 成功解析一帧, -1: 格式错误或校验失败, -2: 数据不足
 */
int protocol_parse_frame(const uint8_t *buffer, uint16_t len, uint8_t *out_ctrl, uint8_t *out_cmd, uint8_t **out_data, uint16_t *out_data_len);

#endif // PROTOCOL_H
