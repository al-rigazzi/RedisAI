#include "../../src/redisai.h"
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include "../../src/util/arr.h"

#define LLAPIMODULE_OK 0
#define LLAPIMODULE_ERR 1

pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t global_cond = PTHREAD_COND_INITIALIZER;

typedef struct DAGRunResults {
    RAI_Tensor **outputs;
    RAI_Error **opsStatus;
} DAGRunResults;

static void *_getFromKeySpace(RedisModuleCtx *ctx, const char *keyNameStr, RedisModuleKey **key) {

    RedisModuleString *keyRedisStr = RedisModule_CreateString(ctx, keyNameStr, strlen(keyNameStr));
    *key = RedisModule_OpenKey(ctx, keyRedisStr, REDISMODULE_READ);
    RedisModule_FreeString(ctx, keyRedisStr);
    return RedisModule_ModuleTypeGetValue(*key);
}

static void _DAGFinishFuncError(RAI_OnFinishCtx *onFinishCtx, void *private_data) {
    //Do nothing, this callback should not be used...
    RedisModule_Assert(false);
}

static void _DAGFinishFunc(RAI_OnFinishCtx *onFinishCtx, void *private_data) {

    DAGRunResults *results = (DAGRunResults *)private_data;
    if (RedisAI_DAGRunError(onFinishCtx)) {
        for (size_t i = 0; i < RedisAI_DAGNumOps(onFinishCtx); i++) {
            RAI_Error *status = RedisAI_DAGCopyOpStatus(onFinishCtx, i);
            results->opsStatus = array_append(results->opsStatus, status);
        }
        pthread_cond_signal(&global_cond);
        return;
    }
    size_t n_outputs = RedisAI_DAGNumOutputs(onFinishCtx);
    for (size_t i = 0; i < n_outputs; i++) {
        RAI_Tensor *t = RedisAI_DAGOutputTensor(onFinishCtx, i);
        RedisModule_Assert(t != NULL);
        results->outputs = array_append(results->outputs, RedisAI_TensorGetShallowCopy(t));
    }
    RAI_Tensor *t = RedisAI_DAGOutputTensor(onFinishCtx, n_outputs);
    RedisModule_Assert(t == NULL);
    pthread_cond_signal(&global_cond);
}

static int _testModelRunOpError(RedisModuleCtx *ctx, RAI_DAGRunCtx *run_info) {

    RAI_Error *err;
    RedisAI_InitError(&err);
    // The model m{1} should exist in key space.
    RedisModuleKey *key;
    RAI_Model *model = (RAI_Model *)_getFromKeySpace(ctx, "m{1}", &key);
    RedisModule_CloseKey(key);
    RAI_DAGRunOp *op = RedisAI_DAGCreateModelRunOp(model);
    RedisAI_DAGRunOpAddInput(op, "first_input");

    // This model expect for 2 inputs not 1.
    int status = RedisAI_DAGAddRunOp(run_info, op, err);
    if (strcmp(RedisAI_GetError(err), "Number of keys given as INPUTS does not match model definition") != 0) {
        RedisAI_FreeError(err);
        return LLAPIMODULE_ERR;
    }
    RedisAI_ClearError(err);
    RedisModule_Assert(status == REDISMODULE_ERR);
    RedisAI_DAGRunOpAddInput(op, "second_input");
    status = RedisAI_DAGAddRunOp(run_info, op, err);
    if (strcmp(RedisAI_GetError(err), "Number of keys given as OUTPUTS does not match model definition") != 0) {
        RedisAI_FreeError(err);
        return LLAPIMODULE_ERR;
    }
    RedisModule_Assert(status == REDISMODULE_ERR);
    RedisAI_FreeError(err);
    RedisAI_DAGRunOpFree(op);
    return LLAPIMODULE_OK;
}

static int _testEmptyDAGError(RedisModuleCtx *ctx, RAI_DAGRunCtx *run_info) {

    RAI_Error* err;
    RedisAI_InitError(&err);
    int res = LLAPIMODULE_ERR;

    RedisModuleKey *key;
    RAI_Tensor *t = (RAI_Tensor *)_getFromKeySpace(ctx, "a{1}", &key);
    RedisModule_CloseKey(key);
    RedisAI_DAGLoadTensor(run_info, "input", t);

    if(RedisAI_DAGRun(run_info, _DAGFinishFuncError, NULL, err) ==
       REDISMODULE_OK) {
        goto cleanup;
    }
    if(strcmp(RedisAI_GetError(err), "ERR DAG is empty") != 0) {
        goto cleanup;
    }
    res = LLAPIMODULE_OK;

    cleanup:
    RedisAI_FreeError(err);
    RedisAI_DAGFree(run_info);
    return res;
}

static int _testKeysMismatchError(RedisModuleCtx *ctx,RAI_DAGRunCtx *run_info) {

    RAI_Error* err;
    RedisAI_InitError(&err);
    int res = LLAPIMODULE_ERR;

    RedisModuleKey *key;
    RAI_Tensor *t = (RAI_Tensor *)_getFromKeySpace(ctx, "a{1}", &key);
    RedisModule_CloseKey(key);
    RedisAI_DAGLoadTensor(run_info, "input", t);

    RedisAI_DAGAddTensorGet(run_info, "non existing tensor", err);
    if(RedisAI_DAGRun(run_info, _DAGFinishFuncError, NULL, err) ==
       REDISMODULE_OK) {
        goto cleanup;
    }
    if(strcmp(RedisAI_GetError(err), "ERR INPUT key cannot be found in DAG") != 0) {
        goto cleanup;
    }
    res = LLAPIMODULE_OK;

    cleanup:
    RedisAI_FreeError(err);
    RedisAI_DAGFree(run_info);
    return res;
}

static int _testBuildDAGFromString(RedisModuleCtx *ctx,RAI_DAGRunCtx *run_info) {

    RAI_Error* err;
    RedisAI_InitError(&err);
    RAI_Tensor **outputs = array_new(RAI_Tensor *, 1);
    RAI_Error **opsStatus = array_new(RAI_Error *, 1);
    DAGRunResults results = {.outputs = outputs, .opsStatus = opsStatus};
    int res = LLAPIMODULE_ERR;

    RedisModuleKey *key;
    RAI_Tensor *t = (RAI_Tensor *)_getFromKeySpace(ctx, "a{1}", &key);
    RedisModule_CloseKey(key);
    RedisAI_DAGLoadTensor(run_info, "input1", t);

    const char *dag_string = "bad string";
    if (RedisAI_DAGAddOpsFromString(run_info, dag_string, err) != REDISMODULE_ERR) {
        goto cleanup;
    }
    if(strcmp(RedisAI_GetError(err), "DAG op should start with: '|>' ") != 0) {
        goto cleanup;
    }
    RedisAI_ClearError(err);
    t = (RAI_Tensor *)_getFromKeySpace(ctx, "b{1}", &key);
    RedisModule_CloseKey(key);
    RedisAI_DAGAddTensorSet(run_info, "input2", t);

    dag_string = "|> AI.MODELRUN m{1} INPUTS input1 input2 OUTPUTS output |> bad_op no_tensor";
    if (RedisAI_DAGAddOpsFromString(run_info, dag_string, err) != REDISMODULE_ERR) {
        goto cleanup;
    }
    if(strcmp(RedisAI_GetError(err), "unsupported command within DAG") != 0) {
        goto cleanup;
    }
    RedisAI_ClearError(err);
    RedisModule_Assert(RedisAI_DAGNumOps(run_info) == 1);
    dag_string = "|> AI.MODELRUN m{1} INPUTS input1 input2 OUTPUTS output |> AI.TENSORGET output";
    if (RedisAI_DAGAddOpsFromString(run_info, dag_string, err) != REDISMODULE_OK) {
        goto cleanup;
    }
    RedisModule_Assert(RedisAI_DAGNumOps(run_info) == 3);
    RedisAI_DAGAddTensorGet(run_info, "input1", err);
    RedisModule_Assert(RedisAI_DAGNumOps(run_info) == 4);

    pthread_mutex_lock(&global_lock);
    if (RedisAI_DAGRun(run_info, _DAGFinishFunc, &results, err) != REDISMODULE_OK) {
        pthread_mutex_unlock(&global_lock);
        goto cleanup;
    }
    // Wait until the onFinish callback returns.
    pthread_cond_wait(&global_cond, &global_lock);
    pthread_mutex_unlock(&global_lock);

    // Verify that we received the expected tensor at the end of the run.
    RedisModule_Assert(array_len(results.outputs) == 2);
    RedisAI_TensorFree(results.outputs[0]);
    RedisAI_TensorFree(results.outputs[1]);
    res = LLAPIMODULE_OK;

    cleanup:
    RedisAI_FreeError(err);
    array_free(results.outputs);
    array_free(results.opsStatus);
    RedisAI_DAGFree(run_info);
    return res;
}

static int _testSimpleDAGRun(RedisModuleCtx *ctx, RAI_DAGRunCtx *run_info) {

    RAI_Error *err;
    RedisAI_InitError(&err);
    RAI_Tensor **outputs = array_new(RAI_Tensor *, 1);
    RAI_Error **opsStatus = array_new(RAI_Error *, 1);
    DAGRunResults results = {.outputs = outputs, .opsStatus = opsStatus};
    int res = LLAPIMODULE_ERR;

    RedisModuleKey *key;
    RAI_Tensor *t = (RAI_Tensor *)_getFromKeySpace(ctx, "a{1}", &key);
    RedisAI_DAGLoadTensor(run_info, "input1", t);
    RedisModule_CloseKey(key);
    t = (RAI_Tensor *)_getFromKeySpace(ctx, "b{1}", &key);
    RedisAI_DAGLoadTensor(run_info, "input2", t);
    RedisModule_CloseKey(key);

    // The model m{1} should exist in key space.
    RAI_Model *model = (RAI_Model *)_getFromKeySpace(ctx, "m{1}", &key);
    RedisModule_CloseKey(key);
    RAI_DAGRunOp *op = RedisAI_DAGCreateModelRunOp(model);
    RedisAI_DAGRunOpAddInput(op, "input1");
    RedisAI_DAGRunOpAddInput(op, "input2");
    RedisAI_DAGRunOpAddOutput(op, "output");
    int status = RedisAI_DAGAddRunOp(run_info, op, err);
    if (status != REDISMODULE_OK) {
        goto cleanup;
    }

    RedisAI_DAGAddTensorGet(run_info, "output", err);
    pthread_mutex_lock(&global_lock);
    if (RedisAI_DAGRun(run_info, _DAGFinishFunc, &results, err) != REDISMODULE_OK) {
        pthread_mutex_unlock(&global_lock);
        goto cleanup;
    }
    // Wait until the onFinish callback returns.
    pthread_cond_wait(&global_cond, &global_lock);
    pthread_mutex_unlock(&global_lock);

    // Verify that we received the expected tensor at the end of the run.
    RedisModule_Assert(array_len(outputs) == 1);
    RAI_Tensor *out_tensor = outputs[0];
    double expceted[4] = {4, 9, 4, 9};
    double val[4];
    for (long long i = 0; i < 4; i++) {
        if(RedisAI_TensorGetValueAsDouble(out_tensor, i, &val[i]) != 0) {
            goto cleanup;
        }
        if (expceted[i] != val[i]) {
            goto cleanup;
        }
    }
    RedisAI_TensorFree(out_tensor);
    res = LLAPIMODULE_OK;

    cleanup:
    RedisAI_FreeError(err);
    array_free(results.outputs);
    array_free(results.opsStatus);
    RedisAI_DAGFree(run_info);
    return res;
}

static int _testSimpleDAGRun2(RedisModuleCtx *ctx, RAI_DAGRunCtx *run_info) {

    RAI_Error *err;
    RedisAI_InitError(&err);
    RAI_Tensor **outputs = array_new(RAI_Tensor *, 1);
    RAI_Error **opsStatus = array_new(RAI_Error *, 1);
    DAGRunResults results = {.outputs = outputs, .opsStatus = opsStatus};
    int res = LLAPIMODULE_ERR;

    RedisModuleKey *key;
    RAI_Tensor *tensor = (RAI_Tensor*)_getFromKeySpace(ctx, "a{1}", &key);
    RedisModule_CloseKey(key);
    RedisAI_DAGAddTensorSet(run_info, "input1", tensor);
    tensor = (RAI_Tensor*)_getFromKeySpace(ctx, "b{1}", &key);
    RedisModule_CloseKey(key);
    RedisAI_DAGAddTensorSet(run_info, "input2", tensor);

    // The script myscript{1} should exist in key space.
    RAI_Script *script = (RAI_Script*)_getFromKeySpace(ctx, "myscript{1}", &key);
    RedisModule_CloseKey(key);
    RAI_DAGRunOp *op = RedisAI_DAGCreateScriptRunOp(script, "bar");
    RedisAI_DAGRunOpAddInput(op, "input1");
    RedisAI_DAGRunOpAddInput(op, "input2");
    RedisAI_DAGRunOpAddOutput(op, "output");
    int status = RedisAI_DAGAddRunOp(run_info, op, err);
    if (status != REDISMODULE_OK) {
        goto cleanup;
    }

    RedisAI_DAGAddTensorGet(run_info, "output", err);
    pthread_mutex_lock(&global_lock);
    if (RedisAI_DAGRun(run_info, _DAGFinishFunc, &results, err) != REDISMODULE_OK) {
        pthread_mutex_unlock(&global_lock);
        goto cleanup;
    }
    // Wait until the onFinish callback returns.
    pthread_cond_wait(&global_cond, &global_lock);
    pthread_mutex_unlock(&global_lock);

    // Verify that we received the expected tensor at the end of the run.
    RedisModule_Assert(array_len(outputs) == 1);
    RAI_Tensor *out_tensor = outputs[0];
    double expceted[4] = {4, 6, 4, 6};
    double val[4];
    for (long long i = 0; i < 4; i++) {
        if(RedisAI_TensorGetValueAsDouble(out_tensor, i, &val[i]) != 0) {
            goto cleanup;
        }
        if (expceted[i] != val[i]) {
            goto cleanup;
        }
    }
    RedisAI_TensorFree(out_tensor);
    res = LLAPIMODULE_OK;

    cleanup:
    RedisAI_FreeError(err);
    array_free(opsStatus);
    array_free(outputs);
    RedisAI_DAGFree(run_info);
    return res;
}

static int _testSimpleDAGRun2Error(RedisModuleCtx *ctx, RAI_DAGRunCtx *run_info) {

    RAI_Error *err;
    RedisAI_InitError(&err);
    RAI_Tensor **outputs = array_new(RAI_Tensor *, 1);
    RAI_Error **opsStatus = array_new(RAI_Error *, 1);
    DAGRunResults results = {.outputs = outputs,.opsStatus = opsStatus};

    int res = LLAPIMODULE_ERR;

    RedisModuleKey *key;
    RAI_Tensor *tensor = (RAI_Tensor*) _getFromKeySpace(ctx, "a{1}", &key);
    RedisModule_CloseKey(key);
    RedisAI_DAGAddTensorSet(run_info, "input1", tensor);

    // The script myscript{1} should exist in key space.
    RAI_Script *script = (RAI_Script*)_getFromKeySpace(ctx, "myscript{1}", &key);
    RedisModule_CloseKey(key);
    RAI_DAGRunOp *op = RedisAI_DAGCreateScriptRunOp(script, "no_function");
    RedisAI_DAGRunOpAddInput(op, "input1");

    RedisAI_DAGRunOpAddOutput(op, "output");
    int status = RedisAI_DAGAddRunOp(run_info, op, err);
    if (status != REDISMODULE_OK) {
        goto cleanup;
    }

    RedisAI_DAGAddTensorGet(run_info, "output", err);
    pthread_mutex_lock(&global_lock);
    if (RedisAI_DAGRun(run_info, _DAGFinishFunc, &results, err) != REDISMODULE_OK) {
        pthread_mutex_unlock(&global_lock);
        goto cleanup;
    }
    // Wait until the onFinish callback returns.
    pthread_cond_wait(&global_cond, &global_lock);
    pthread_mutex_unlock(&global_lock);

    // Verify that we received an error in SCRIPTRUN op.
    RedisModule_Assert(array_len(results.outputs) == 0);
    RedisModule_Assert(array_len(results.opsStatus) == 3);
    RAI_Error *op_status = results.opsStatus[0];
    if (RedisAI_GetErrorCode(op_status) != RedisAI_ErrorCode_OK) goto cleanup;
    RedisAI_FreeError(op_status);
    op_status = results.opsStatus[1];
    if (RedisAI_GetErrorCode(op_status) != RedisAI_ErrorCode_ESCRIPTRUN) goto cleanup;
    RedisAI_FreeError(op_status);
    op_status = results.opsStatus[2];
    if (RedisAI_GetErrorCode(op_status) != RedisAI_ErrorCode_OK) goto cleanup;
    RedisAI_FreeError(op_status);

    res = LLAPIMODULE_OK;

    cleanup:
    RedisAI_FreeError(err);
    array_free(results.outputs);
    array_free(results.opsStatus);
    RedisAI_DAGFree(run_info);
    return res;
}

int RAI_llapi_DAGRun(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);

    if(argc > 1) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }
    RAI_DAGRunCtx *run_info = RedisAI_DAGRunCtxCreate();

    // Test the case of a failure due to addition of a non compatible MODELRUN op.
    if(_testModelRunOpError(ctx, run_info) != LLAPIMODULE_OK) {
        RedisAI_DAGFree(run_info);
        return RedisModule_ReplyWithSimpleString(ctx, "MODELRUN op error test failed");
    }
    // Test the case of a failure due an empty DAG.
    if(_testEmptyDAGError(ctx, run_info) != LLAPIMODULE_OK) {
        RedisAI_DAGFree(run_info);
        return RedisModule_ReplyWithSimpleString(ctx, "DAG keys mismatch error test failed");
    }
    run_info = RedisAI_DAGRunCtxCreate();
    // Test the case of a failure due to an op within a DAG whose inkey does not exist in the DAG.
    if(_testKeysMismatchError(ctx, run_info) != LLAPIMODULE_OK) {
        RedisAI_DAGFree(run_info);
        return RedisModule_ReplyWithSimpleString(ctx, "DAG keys mismatch error test failed");
    }
    run_info = RedisAI_DAGRunCtxCreate();
    // Test the case of building and running a DAG with LOAD, TENSORGET and MODELRUN ops.
    if(_testSimpleDAGRun(ctx, run_info) != LLAPIMODULE_OK) {
        return RedisModule_ReplyWithSimpleString(ctx, "Simple DAG run test failed");
    }
    run_info = RedisAI_DAGRunCtxCreate();
    // Test the case of building and running a DAG with TENSORSET, SCRIPTRUN and TENSORGET ops.
    if(_testSimpleDAGRun2(ctx, run_info) != LLAPIMODULE_OK) {
        return RedisModule_ReplyWithSimpleString(ctx, "Simple DAG run2 test failed");
    }
    run_info = RedisAI_DAGRunCtxCreate();
    // Test the case of building the same DAG as in previous test, but when this time it should return with an error.
    if(_testSimpleDAGRun2Error(ctx, run_info) != LLAPIMODULE_OK) {
        return RedisModule_ReplyWithSimpleString(ctx, "Simple DAG run2 error test failed");
    }
    run_info = RedisAI_DAGRunCtxCreate();
    // Test the case of building DAG ops from string.
    if(_testBuildDAGFromString(ctx, run_info) != LLAPIMODULE_OK) {
        return RedisModule_ReplyWithSimpleString(ctx, "Build DAG from string test failed");
    }
    return RedisModule_ReplyWithSimpleString(ctx, "DAG run success");
}

int RAI_llapi_DAG_resnet(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);

    if(argc > 1) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }
    RAI_Error* err;
    RedisAI_InitError(&err);
    RAI_Tensor** outputs = array_new(RAI_Tensor *, 1);
    RAI_Error** opsStatus = array_new(RAI_Error *, 1);
    DAGRunResults results = {.outputs = outputs, .opsStatus = opsStatus};
    const char *test_res = "DAG resnet failed";

    // Build the DAG with LOAD->SCRIPTRUN->MODELRUN->MODELRUN-SCRIPTRUN->SCRIPTRUN->TENSORGET
    RAI_DAGRunCtx *run_info = RedisAI_DAGRunCtxCreate();
    RedisModuleKey *key;
    RAI_Tensor *t = (RAI_Tensor *)_getFromKeySpace(ctx, "image:{{1}}", &key);
    RedisAI_DAGLoadTensor(run_info, "image:{{1}}", t);
    RedisModule_CloseKey(key);

    RAI_Script *script = (RAI_Script *)_getFromKeySpace(ctx,  "imagenet_script1:{{1}}", &key);
    RedisModule_CloseKey(key);
    RAI_DAGRunOp *script_op = RedisAI_DAGCreateScriptRunOp(script, "pre_process_3ch");
    RedisAI_DAGRunOpAddInput(script_op, "image:{{1}}");
    RedisAI_DAGRunOpAddOutput(script_op, "tmp_key:{{1}}");
    RedisAI_DAGAddRunOp(run_info, script_op, err);

    RAI_Model *model = (RAI_Model *)_getFromKeySpace(ctx,  "imagenet_model1:{{1}}", &key);
    RedisModule_CloseKey(key);
    RAI_DAGRunOp *model_op = RedisAI_DAGCreateModelRunOp(model);
    RedisAI_DAGRunOpAddInput(model_op, "tmp_key:{{1}}");
    RedisAI_DAGRunOpAddOutput(model_op, "tmp_key2_0");
    int status = RedisAI_DAGAddRunOp(run_info, model_op, err);
    if (status != REDISMODULE_OK) goto cleanup;

    model = (RAI_Model *)_getFromKeySpace(ctx,  "imagenet_model2:{{1}}", &key);
    RedisModule_CloseKey(key);
    model_op = RedisAI_DAGCreateModelRunOp(model);
    RedisAI_DAGRunOpAddInput(model_op, "tmp_key:{{1}}");
    RedisAI_DAGRunOpAddOutput(model_op, "tmp_key2_1");
    status = RedisAI_DAGAddRunOp(run_info, model_op, err);
    if (status != REDISMODULE_OK) goto cleanup;

    script_op = RedisAI_DAGCreateScriptRunOp(script, "ensemble");
    RedisAI_DAGRunOpAddInput(script_op, "tmp_key2_0");
    RedisAI_DAGRunOpAddInput(script_op, "tmp_key2_1");
    RedisAI_DAGRunOpAddOutput(script_op, "tmp_key_1:{{1}}");
    RedisAI_DAGAddRunOp(run_info, script_op, err);

    script_op = RedisAI_DAGCreateScriptRunOp(script, "post_process");
    RedisAI_DAGRunOpAddInput(script_op, "tmp_key_1:{{1}}");
    RedisAI_DAGRunOpAddOutput(script_op, "output:{{1}}");
    RedisAI_DAGAddRunOp(run_info, script_op, err);

    RedisAI_DAGAddTensorGet(run_info, "output:{{1}}", err);

    pthread_mutex_lock(&global_lock);
    if (RedisAI_DAGRun(run_info, _DAGFinishFunc, &results, err) != REDISMODULE_OK) {
        pthread_mutex_unlock(&global_lock);
        goto cleanup;
    }
    // Wait until the onFinish callback returns.
    pthread_cond_wait(&global_cond, &global_lock);
    pthread_mutex_unlock(&global_lock);

    // Verify that we received the expected output.
    RedisModule_Assert(array_len(results.outputs) == 1);
    RAI_Tensor *out_tensor = outputs[0];
    long long val;
    if(RedisAI_TensorGetValueAsLongLong(out_tensor, 0, &val) != 0) goto cleanup;
    RedisAI_TensorFree(out_tensor);
    if (0<= val && val <= 1000) {
        test_res = "DAG resnet success";
    }

    cleanup:
    RedisAI_FreeError(err);
    array_free(results.outputs);
    array_free(results.opsStatus);
    RedisAI_DAGFree(run_info);
    return RedisModule_ReplyWithSimpleString(ctx, test_res);
}
