#pragma once

/* OpenBeken Native API (OBKA), protocol version 1. */
void DRV_OpenBeken_API_Init(void);
void DRV_OpenBeken_API_Deinit(void);
void DRV_OpenBeken_API_OnEverySecond(void);
void DRV_OpenBeken_API_OnChannelChanged(int channel, int value);
/* Called by the logical LED layer, not by individual PWM channels. */
void DRV_OpenBeken_API_OnLightChanged(void);
int DRV_OpenBeken_API_GetPort(void);
