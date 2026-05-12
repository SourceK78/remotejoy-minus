#ifndef RJM_LOG_H
#define RJM_LOG_H

void rjmLogInit(void);
void rjmLogText(const char *text);
void rjmLogHex(const char *label, int value);

#endif
