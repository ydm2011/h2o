/*
 * Copyright (c) 2015 DeNA Co., Ltd., Kazuho Oku
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include <string.h>
#include "h2o.h"
#include "h2o/configurator.h"

struct headers_configurator_t {
    h2o_configurator_t super;
    H2O_VECTOR(h2o_headers_command_t) * cmds, _cmd_stack[H2O_CONFIGURATOR_NUM_LEVELS + 1];
};

static int extract_name(const char *src, size_t len, h2o_iovec_t **_name)
{
    h2o_iovec_t name;
    const h2o_token_t *name_token;

    name = h2o_str_stripws(src, len);
    if (name.len == 0)
        return -1;

    name = h2o_strdup(NULL, name.base, name.len);
    h2o_strtolower(name.base, name.len);

    if ((name_token = h2o_lookup_token(name.base, name.len)) != NULL) {
        *_name = (h2o_iovec_t *)&name_token->buf;
        free(name.base);
    } else {
        *_name = h2o_mem_alloc(sizeof(**_name));
        **_name = name;
    }

    return 0;
}

static int extract_name_value(const char *src, h2o_iovec_t **name, h2o_iovec_t *value)
{
    const char *colon = strchr(src, ':');

    if (colon == NULL)
        return -1;

    if (extract_name(src, colon - src, name) != 0)
        return -1;
    *value = h2o_str_stripws(colon + 1, strlen(colon + 1));
    *value = h2o_strdup(NULL, value->base, value->len);

    return 0;
}

static void add_cmd(struct headers_configurator_t *self, int cmd_id, h2o_iovec_t *name, h2o_iovec_t value)
{
    h2o_vector_reserve(NULL, (h2o_vector_t *)self->cmds, sizeof(self->cmds->entries[0]), self->cmds->size + 1);
    self->cmds->entries[self->cmds->size++] = (h2o_headers_command_t){cmd_id, name, value};
}

static int on_config_header_2arg(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, int cmd_id, yoml_t *node)
{
    struct headers_configurator_t *self = (void *)cmd->configurator;
    h2o_iovec_t *name, value;

    if (extract_name_value(node->data.scalar, &name, &value) != 0) {
        h2o_configurator_errprintf(cmd, node, "failed to parse the value; should be in form of `name: value`");
        return -1;
    }
    add_cmd(self, cmd_id, name, value);
    return 0;
}

#define DEFINE_2ARG(fn, cmd_id)                                                                                                    \
    static int fn(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)                                  \
    {                                                                                                                              \
        return on_config_header_2arg(cmd, ctx, cmd_id, node);                                                                      \
    }

DEFINE_2ARG(on_config_header_add, H2O_HEADERS_CMD_ADD)
DEFINE_2ARG(on_config_header_append, H2O_HEADERS_CMD_APPEND)
DEFINE_2ARG(on_config_header_merge, H2O_HEADERS_CMD_MERGE)
DEFINE_2ARG(on_config_header_set, H2O_HEADERS_CMD_SET)
DEFINE_2ARG(on_config_header_setifempty, H2O_HEADERS_CMD_SETIFEMPTY)

#undef DEFINE_2ARG

static int on_config_header_unset(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    struct headers_configurator_t *self = (void *)cmd->configurator;
    h2o_iovec_t *name;

    if (extract_name(node->data.scalar, strlen(node->data.scalar), &name) != 0) {
        h2o_configurator_errprintf(cmd, node, "invalid header name");
        return -1;
    }
    add_cmd(self, H2O_HEADERS_CMD_UNSET, name, (h2o_iovec_t){});
    return 0;
}

static int on_config_enter(h2o_configurator_t *_self, h2o_configurator_context_t *ctx, yoml_t *node)
{
    struct headers_configurator_t *self = (void *)_self;

    h2o_vector_reserve(NULL, (h2o_vector_t *)&self->cmds[1], sizeof(self->cmds[0].entries[0]), self->cmds[0].size);
    memcpy(self->cmds[1].entries, self->cmds[0].entries, sizeof(self->cmds->entries[0]) * self->cmds->size);
    self->cmds[1].size = self->cmds[0].size;
    ++self->cmds;
    return 0;
}

static int on_config_exit(h2o_configurator_t *_self, h2o_configurator_context_t *ctx, yoml_t *node)
{
    struct headers_configurator_t *self = (void *)_self;

    if (ctx->pathconf != NULL && self->cmds->size != 0) {
        h2o_vector_reserve(NULL, (h2o_vector_t *)self->cmds, sizeof(self->cmds->entries[0]), sizeof(self->cmds->size));
        self->cmds->entries[self->cmds->size] = (h2o_headers_command_t){H2O_HEADERS_CMD_NULL};
        h2o_headers_register(ctx->pathconf, self->cmds->entries);
    } else {
        free(self->cmds->entries);
        memset(self->cmds, 0, sizeof(*self->cmds));
    }

    --self->cmds;
    return 0;
}

void h2o_headers_register_configurator(h2o_globalconf_t *conf)
{
    struct headers_configurator_t *c = (void *)h2o_configurator_create(conf, sizeof(*c));

    c->super.enter = on_config_enter;
    c->super.exit = on_config_exit;
#define DEFINE_CMD(name, cb, desc)                                                                                                 \
    h2o_configurator_define_command(&c->super, name, H2O_CONFIGURATOR_FLAG_GLOBAL | H2O_CONFIGURATOR_FLAG_HOST |                   \
                                                         H2O_CONFIGURATOR_FLAG_PATH | H2O_CONFIGURATOR_FLAG_EXPECT_SCALAR,         \
                                    cb, desc)
    DEFINE_CMD("header.add", on_config_header_add, "adds a new header line to the response headers");
    DEFINE_CMD("header.append", on_config_header_append,
               "adds a new header line, or appends the value to the existing header with the same name (separated by `,`)");
    DEFINE_CMD("header.merge", on_config_header_merge,
               "adds a new header line, or merges the value to the existing header of comma-separated values");
    DEFINE_CMD("header.set", on_config_header_set, "sets a header line, removing headers with the same name (if exist)");
    DEFINE_CMD("header.setifempty", on_config_header_setifempty,
               "sets a header line, only when a header with the same name does not exist");
    DEFINE_CMD("header.unset", on_config_header_unset, "removes headers with the specified name");
#undef DEFINE_CMD

    c->cmds = c->_cmd_stack;
}