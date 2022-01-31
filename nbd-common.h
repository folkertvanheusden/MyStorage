#define NBD_CMD_READ		0
#define NBD_CMD_WRITE		1
#define NBD_CMD_DISC		2
#define NBD_CMD_FLUSH 		3
#define NBD_CMD_TRIM		4
#define NBD_CMD_CACHE		5
#define NBD_CMD_WRITE_ZEROES	6
extern const char *const nbd_cmd_names[];

#define NBD_CMD_FLAG_FUA	(1 << 0)

#define NBD_FLAG_C_FIXED_NEWSTYLE (1 << 0)
#define NBD_FLAG_C_NO_ZEROES    (1 << 1)
#define NBD_FLAG_HAS_FLAGS      (1 << 0);
#define NBD_FLAG_SEND_FLUSH	(1 << 2)
#define NBD_FLAG_SEND_FUA	(1 << 3)
#define NBD_FLAG_SEND_TRIM      (1 << 5)
#define NBD_FLAG_SEND_WRITE_ZEROES (1 << 6)
#define NBD_FLAG_CAN_MULTI_CONN	(1 << 8)

#define NBD_INFO_BLOCK_SIZE	3
#define NBD_INFO_EXPORT		0

#define NBD_OPT_EXPORT_NAME	1
#define NBD_OPT_GO		7
#define NBD_OPT_STRUCTURED_REPLY 8

#define NBD_REP_ACK             1
#define NBD_REP_ERR_UNSUP	(1 | NBD_REP_FLAG_ERROR)
#define NBD_REP_FLAG_ERROR      (1 << 31)
#define NBD_REP_INFO		3

