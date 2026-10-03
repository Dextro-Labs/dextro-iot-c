#include <dextro-iot/iot_types.h>

const char *iot_err_str(iot_err_t err)
{
    switch (err) {
    case IOT_OK:
        return "ok";
    case IOT_ERR_ARG:
        return "invalid argument";
    case IOT_ERR_NO_SPACE:
        return "no space";
    case IOT_ERR_PARSE:
        return "parse error";
    case IOT_ERR_LIMIT:
        return "limit exceeded";
    case IOT_ERR_NOT_FOUND:
        return "not found";
    case IOT_ERR_TYPE:
        return "type mismatch";
    case IOT_ERR_STATE:
        return "invalid state";
    case IOT_ERR_TRANSPORT:
        return "transport error";
    case IOT_ERR_TIMEOUT:
        return "timeout";
    }
    return "unknown";
}
