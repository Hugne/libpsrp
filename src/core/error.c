#include "psrp/psrp.h"

const char *psrp_strerror(psrp_result_t rc)
{
    switch (rc) {
    case PSRP_OK:               return "ok";
    case PSRP_ERR_INVALID_ARG:  return "invalid argument";
    case PSRP_ERR_NOMEM:        return "out of memory";
    case PSRP_ERR_TOO_SMALL:    return "buffer too small";
    case PSRP_ERR_STATE:        return "invalid state for this operation";
    case PSRP_ERR_NOT_FOUND:    return "not found";
    case PSRP_ERR_MALFORMED:    return "malformed data";
    case PSRP_ERR_TRUNCATED:    return "truncated: more data needed";
    case PSRP_ERR_OVERFLOW:     return "value exceeds limit";
    case PSRP_ERR_UNSUPPORTED:  return "unsupported";
    case PSRP_ERR_TRANSPORT:    return "transport error";
    case PSRP_ERR_AUTH:         return "authentication failed";
    case PSRP_ERR_UNREACHABLE:  return "endpoint unreachable";
    case PSRP_ERR_CRYPTO:       return "crypto error";
    case PSRP_ERR_XML:          return "xml error";
    case PSRP_ERR_INTERNAL:     return "internal error";
    }
    return "unknown error";
}

const char *psrp_version_string(void)
{
    return "0.1.0";
}
