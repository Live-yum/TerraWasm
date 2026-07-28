#ifndef TERRA_STATUS_H
#define TERRA_STATUS_H

/* Stable public status codes shared by the opaque API and internal parser. */
typedef enum terrax_world_status {
    TERRAX_WORLD_STATUS_OK                = 0,
    TERRAX_WORLD_STATUS_INVALID_ARGUMENT  = 1,
    TERRAX_WORLD_STATUS_BUFFER_TOO_SMALL  = 2,
    TERRAX_WORLD_STATUS_NOT_FOUND         = 3,
    TERRAX_WORLD_STATUS_NOT_SUPPORTED     = 4,
    TERRAX_WORLD_STATUS_PARSE_ERROR       = 5,
    TERRAX_WORLD_STATUS_VALIDATION_ERROR  = 6,
    TERRAX_WORLD_STATUS_IO_ERROR          = 7,
    TERRAX_WORLD_STATUS_STATE_ERROR       = 8,
    TERRAX_WORLD_STATUS_INTERNAL_ERROR    = 9
} terrax_world_status;

#endif /* TERRA_STATUS_H */
