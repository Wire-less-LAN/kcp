//=====================================================================
//
// test.cpp - kcp 测试用例
//
// 说明：
// gcc test.cpp -o test -lstdc++
//
//=====================================================================

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#include <string>
#include <fstream>
#include <iostream>

#include "test.h"
#include "ikcp.c"
#include "ikcp.h"


int wnd_size;
double rto_priv;
int cwnd_priv;
int loss;

std::ofstream out("ikcp.log", std::ios::app);
void write_log(const char *log, struct IKCPCB *kcp, void *user) {
	std::string log_str(log);
	if (user == (void*)0) {
		out << "[1] ";
	} else {
		out << "[2] ";
	}
	out << log_str << std::endl;
}

// 模拟网络
LatencySimulator *vnet;

// 模拟网络：模拟发送一个 udp包
int udp_output(const char *buf, int len, ikcpcb *kcp, void *user)
{
	union { int id; void *ptr; } parameter;
	parameter.ptr = user;
	vnet->send(parameter.id, buf, len);
	return 0;
}

struct test_res{
	int avgrtt, maxrtt, tx1;
	int p_avgrtt, p_maxrtt, p_tx;
	int up_avgrtt, up_maxrtt, up_tx;
};

// 测试用例
test_res test(bool prio_test)
{


	// 创建两个端点的 kcp对象，第一个参数 conv是会话编号，同一个会话需要相同
	// 最后一个是 user参数，用来传递标识
	ikcpcb *kcp1 = ikcp_create(0x11223344, (void*)0);
	ikcpcb *kcp2 = ikcp_create(0x11223344, (void*)1);

	// 设置kcp的下层输出，这里为 udp_output，模拟udp网络输出函数
	kcp1->output = udp_output;
	kcp2->output = udp_output;

	// kcp1->logmask = 0xfff;
	// kcp2->logmask = 0xfff;
	// kcp1->writelog = write_log;
	// kcp2->writelog = write_log;

	IUINT32 current = iclock();
	IUINT32 slap = current + 20;
	IUINT32 index = 0;
	IUINT32 next = 0;
	IINT64 sumrtt = 0;
	int count = 0;
	int maxrtt = 0;

	IINT64 up_sumrtt = 0;
	IINT64 p_sumrtt = 0;
	int up_maxrtt = 0;
	int p_maxrtt = 0;
	int up_count = 0;
	int p_count = 0;

	// 配置窗口大小：平均延迟200ms，每20ms发送一个包，
	// 而考虑到丢包重发，设置最大收发窗口为128
	ikcp_wndsize(kcp1, 128, 128);
	ikcp_wndsize(kcp2, 128, 128);

	// 启动快速模式
	// 第二个参数 nodelay-启用以后若干常规加速将启动
	// 第三个参数 interval为内部处理时钟，默认设置为 10ms
	// 第四个参数 resend为快速重传指标，设置为2
	// 第五个参数 为是否禁用常规流控，这里禁止
	ikcp_nodelay(kcp1, 2, 10, 2, 1);
	ikcp_nodelay(kcp2, 2, 10, 2, 1);

	char buffer[2000];
	int hr;
	
	srand(iclock());
	bool prios[5001];
	if (prio_test) {
		for (int i = 0; i <= 5000; ++i) {
			if (rand() % 100 > 0) {
				prios[i] = 0;
			} else {
				for (int j = i; j <=5000 && j < i + 10; ++j) {
					prios[j] = 1;
				}
				i += 10;
			}
			// prios[i] = rand() % 10 > 0 ? 0 : 1;
		}
	} else {
		for (int i = 0; i <= 5000; ++i) {
			prios[i] = 0;
		}
	}
	int seg_ptr = 0;

	IUINT32 ts1 = iclock();

	while (1) {
		isleep(1);
		current = iclock();
		ikcp_update(kcp1, iclock());
		ikcp_update(kcp2, iclock());

		// 每隔 20ms，kcp1发送数据
		for (; current >= slap; slap += 20) {
			((IUINT32*)buffer)[0] = index++;
			((IUINT32*)buffer)[1] = current;
			((IUINT8*)buffer)[8] = prios[seg_ptr];

			// 发送上层协议包
			ikcp_send(kcp1, buffer, 9, prios[seg_ptr++]);
		}

		// 处理虚拟网络：检测是否有udp包从p1->p2
		while (1) {
			hr = vnet->recv(1, buffer, 2000);
			if (hr < 0) break;
			// 如果 p2收到udp，则作为下层协议输入到kcp2
			ikcp_input(kcp2, buffer, hr);
		}

		// 处理虚拟网络：检测是否有udp包从p2->p1
		while (1) {
			hr = vnet->recv(0, buffer, 2000);
			if (hr < 0) break;
			// 如果 p1收到udp，则作为下层协议输入到kcp1
			ikcp_input(kcp1, buffer, hr);
		}

		// kcp2接收到任何包都返回回去
		while (1) {
			hr = ikcp_recv(kcp2, buffer, 10);
			// 没有收到包就退出
			if (hr < 0) break;
			// 如果收到包就回射
			ikcp_send(kcp2, buffer, hr, 0);
		}

		// kcp1收到kcp2的回射数据
		while (1) {
			hr = ikcp_recv(kcp1, buffer, 10);
			// 没有收到包就退出
			if (hr < 0) break;
			IUINT32 sn = *(IUINT32*)(buffer + 0);
			IUINT32 ts = *(IUINT32*)(buffer + 4);
			IUINT8 prio = *(IUINT8*)(buffer + 8);
			IUINT32 rtt = current - ts;
			
			if (sn != next) {
				// 如果收到的包不连续
				printf("ERROR sn %d<->%d\n", (int)count, (int)next);
				return {0, 0, 0, 0, 0, 0, 0, 0, 0};
			}

			next++;
			sumrtt += rtt;
			count++;
			if (rtt > (IUINT32)maxrtt) maxrtt = rtt;

			if (prio) {
				p_sumrtt += rtt;
				p_count++;
				if (rtt > (IUINT32)p_maxrtt) p_maxrtt = rtt;
			} else {
				up_sumrtt += rtt;
				up_count ++;
				if (rtt > (IUINT32)up_maxrtt) up_maxrtt = rtt;
			}

			// printf("[RECV] mode=%d sn=%d rtt=%d prio=%d\n", mode, (int)sn, (int)rtt, (int)prio);
		}
		if (next > 700) break;
	}

	ts1 = iclock() - ts1;

	ikcp_release(kcp1);
	ikcp_release(kcp2);

	return {(int)(sumrtt / count), (int)maxrtt, (int)vnet->tx1,
		(int)(p_sumrtt / p_count), (int)p_maxrtt, (int)p_count,
	 	(int)(up_sumrtt / up_count), (int)up_maxrtt, (int)up_count};
}

int main()
{
	int fd;

	// rto_cwnd_test
 	fd = open("/workspaces/kcp/rto_cwnd_result.txt", O_WRONLY | O_CREAT | O_APPEND, 0644); 
    if (fd == -1) {
        perror("open");
		exit(1);
    }
	for (rto_priv = 0.1; rto_priv < 1; rto_priv += 0.1) {
		for (cwnd_priv = 0; cwnd_priv < 10; ++cwnd_priv) {
			vnet = new OldLatencySimulator(10, 60, 125);
			auto res = test(true);

			dprintf(fd, "(rto_priv=%.1f, cwnd_priv=%d): \n", rto_priv, cwnd_priv);
			dprintf(fd, "\tavgrtt=%d maxrtt=%d tx=%d\n", res.avgrtt, res.maxrtt, res.tx1);
			dprintf(fd, "\tprio: avgrtt=%d maxrtt=%d tx=%d\n", res.p_avgrtt, res.p_maxrtt, res.p_tx);
			dprintf(fd, "\tunprio: avgrtt=%d maxrtt=%d tx=%d\n\n", res.up_avgrtt, res.up_maxrtt, res.up_tx);
			delete vnet;
		}
	}
	close(fd);

	// rto_loss_test
 	fd = open("/workspaces/kcp/rto_loss_result.txt", O_WRONLY | O_CREAT | O_APPEND, 0644); 
    if (fd == -1) {
        perror("open");
		exit(1);
    }
	for (rto_priv = 0.1; rto_priv < 1; rto_priv += 0.1) {
		for (loss = 10; loss < 100; loss += 10) {
			vnet = new OldLatencySimulator(loss, 60, 125);
			auto res = test(true);

			dprintf(fd, "(rto_priv=%.1f, loss=%d): \n", rto_priv, loss);
			dprintf(fd, "\tavgrtt=%d maxrtt=%d tx=%d\n", res.avgrtt, res.maxrtt, res.tx1);
			dprintf(fd, "\tprio: avgrtt=%d maxrtt=%d tx=%d\n", res.p_avgrtt, res.p_maxrtt, res.p_tx);
			dprintf(fd, "\tunprio: avgrtt=%d maxrtt=%d tx=%d\n\n", res.up_avgrtt, res.up_maxrtt, res.up_tx);
			delete vnet;
		}
	}
	close(fd);

	// vib_rto_cwnd_test
 	fd = open("/workspaces/kcp/vib_rto_cwnd_result.txt", O_WRONLY | O_CREAT | O_APPEND, 0644); 
    if (fd == -1) {
        perror("open");
		exit(1);
    }
	for (rto_priv = 0.1; rto_priv < 1; rto_priv += 0.1) {
		for (cwnd_priv = 0; cwnd_priv < 10; ++cwnd_priv) {
			vnet = new VibLatencySimulator(10, 40, 60, 125, 100, 1);
			auto res = test(true);

			dprintf(fd, "(rto_priv=%.1f, cwnd_priv=%d): \n", rto_priv, cwnd_priv);
			dprintf(fd, "\tavgrtt=%d maxrtt=%d tx=%d\n", res.avgrtt, res.maxrtt, res.tx1);
			dprintf(fd, "\tprio: avgrtt=%d maxrtt=%d tx=%d\n", res.p_avgrtt, res.p_maxrtt, res.p_tx);
			dprintf(fd, "\tunprio: avgrtt=%d maxrtt=%d tx=%d\n\n", res.up_avgrtt, res.up_maxrtt, res.up_tx);
			delete vnet;
		}
	}
	close(fd);

	// no prio mechanism test
	vnet = new VibLatencySimulator(10, 40, 60, 125, 100, 1);
	auto res = test(false);
	printf("\tavgrtt=%d maxrtt=%d tx=%d\n", res.avgrtt, res.maxrtt, res.tx1);

	return 0;
}