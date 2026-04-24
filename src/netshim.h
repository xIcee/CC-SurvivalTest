#ifndef CCST_NETSHIM_H
#define CCST_NETSHIM_H

/* Wraps selected Protocol.Handlers (see ref/hax.c pattern) for server-driven survival hooks. */
void CCST_Netshim_Init(void);
void CCST_Netshim_Free(void);
/* Call after Protocol reset (e.g. IGameComponent OnReset) so the wrapper is re-applied. */
void CCST_Netshim_Refresh(void);

#endif
