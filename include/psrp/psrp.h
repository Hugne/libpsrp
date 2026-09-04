/* psrp.h - umbrella header for libpsrp.
 *
 * libpsrp is a C implementation of the PowerShell Remoting Protocol
 * ([MS-PSRP]), client side. The protocol core performs no I/O: it is a state
 * machine that consumes and produces bytes, with transport, crypto and host
 * callbacks injected by the caller.
 */
#ifndef PSRP_H
#define PSRP_H

#include "psrp/psrp_error.h"
#include "psrp/psrp_types.h"
#include "psrp/psrp_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PSRP_VERSION_MAJOR 0
#define PSRP_VERSION_MINOR 1
#define PSRP_VERSION_PATCH 0

/* "0.1.0" */
const char *psrp_version_string(void);

#ifdef __cplusplus
}
#endif

#endif /* PSRP_H */
