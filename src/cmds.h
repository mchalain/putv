#ifndef __CMDS_H__
#define __CMDS_H__

#ifndef CMDS_CTX
typedef void cmds_ctx_t;
#endif
typedef struct cmds_ops_s cmds_ops_t;
struct cmds_ops_s
{
	cmds_ctx_t *(*init)(player_ctx_t *, media_t *, void *arg);
	int (*run)(cmds_ctx_t *);
	void (*destroy)(cmds_ctx_t *);
};

typedef struct cmds_s cmds_t;
struct cmds_s
{
	cmds_ops_t *ops;
	cmds_ctx_t *ctx;
};

extern cmds_ops_t *cmds_line;
extern cmds_ops_t *cmds_json;
extern cmds_ops_t *cmds_input;
#endif
