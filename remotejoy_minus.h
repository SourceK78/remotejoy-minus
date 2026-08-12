#ifndef REMOTEJOY_MINUS_H
#define REMOTEJOY_MINUS_H

#define RJM_ASYNC_JOY 4

void rjmUsbRegister(void);
void rjmUsbUnregister(void);
int rjmUsbStart(void);
int rjmUsbStop(void);
void rjmUsbShutdown(void);
int rjmUsbSuspend(void);
int rjmUsbResume(void);
int rjmUsbWaitForConnect(void);
int rjmUsbIsConnected(void);
void rjmUsbSetPopsContext(int is_pops);
void rjmUsbAsyncFlush(void);
int rjmUsbAsyncRead(unsigned char *data, int len);

#endif
