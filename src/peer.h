#ifndef CCST_PEER_H
#define CCST_PEER_H
#include "Core.h"

void CCST_Peer_Init(void);
void CCST_Peer_Free(void);
void CCST_Peer_Refresh(void);
void CCST_Peer_SendDeath(int cause, int inciterEntityId);

#endif
