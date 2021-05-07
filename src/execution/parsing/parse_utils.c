#include <execution/utils.h>
#include "parse_utils.h"
#include "string.h"

int ParseTimeout(RedisModuleString *timeout_arg, RAI_Error *error, long long *timeout) {

    const int retval = RedisModule_StringToLongLong(timeout_arg, timeout);
    if (retval != REDISMODULE_OK || *timeout <= 0) {
        RAI_SetError(error, RAI_EMODELRUN, "ERR Invalid value for TIMEOUT");
        return REDISMODULE_ERR;
    }
    return REDISMODULE_OK;
}

const char *ScriptCommand_GetFunctionName(RedisModuleString *functionName) {
    const char *functionName_cstr = RedisModule_StringPtrLen(functionName, NULL);
    return functionName_cstr;
}

int ParseKeysArgs(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, RAI_Error *err) {
    if (argc < 3) {
        RAI_SetError(err, RAI_EDAGBUILDER, "ERR Missing arguments after KEYS keyword");
        return -1;
    }

    long long n_keys;
    const int retval = RedisModule_StringToLongLong(argv[1], &n_keys);
    if (retval != REDISMODULE_OK || n_keys <= 0) {
        RAI_SetError(err, RAI_EDAGBUILDER, "ERR Invalid or negative value found in number of KEYS");
        return -1;
    }

    // Go over the given args and verify that these keys are located in the local shard.
    int key_pos = 0;
    for (size_t argpos = 2; (argpos < argc) && (key_pos < n_keys); argpos++) {
        if (!VerifyKeyInThisShard(ctx, argv[argpos])) {
            RAI_SetError(err, RAI_EDAGBUILDER, "ERR CROSSSLOT Keys don't hash to the same slot");
            return -1;
        }
        key_pos++;
    }
    if (key_pos != n_keys) {
        RAI_SetError(
            err, RAI_EDAGBUILDER,
            "ERR Number of pre declared KEYS to be used in the command does not match the number "
            "of given arguments");
        return -1;
    }
    return key_pos + 2;
}
