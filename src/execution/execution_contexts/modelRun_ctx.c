
#include "modelRun_ctx.h"
#include "util/string_utils.h"
#include "execution/utils.h"
#include "execution/DAG/dag.h"
#include "execution/run_info.h"
#include "backends/backends.h"

RAI_ModelRunCtx *RAI_ModelRunCtxCreate(RAI_Model *model) {
    RAI_ModelRunCtx *mctx = RedisModule_Calloc(1, sizeof(*mctx));
    mctx->base = RAI_ExecutionCtx_New(RAI_ModelRunCtxFree);
    mctx->model = RAI_ModelGetShallowCopy(model);
    return mctx;
}

int RAI_ModelRunCtxAddInput(RAI_ModelRunCtx *mctx, const char *inputName, RAI_Tensor *inputTensor) {
    RAI_ExecutionCtx_AddInput(mctx->base, inputTensor);
    return 1;
}

int RAI_ModelRunCtxAddOutput(RAI_ModelRunCtx *mctx, const char *outputName) {
    RAI_ExecutionCtx_AddOutput(mctx->base, NULL);
}

inline size_t RAI_ModelRunCtxNumInputs(RAI_ModelRunCtx *mctx) { return RAI_ExecutionCtx_InputsLen(mctx->base); }

inline size_t RAI_ModelRunCtxNumOutputs(RAI_ModelRunCtx *mctx) { return RAI_ExecutionCtx_OutputsLen(mctx->base);}

inline RAI_Tensor *RAI_ModelRunCtxInputTensor(RAI_ModelRunCtx *mctx, size_t index) {
    return RAI_ExecutionCtx_GetInput(mctx->base, index);
}

inline RAI_Tensor *RAI_ModelRunCtxOutputTensor(RAI_ModelRunCtx *mctx, size_t index) {
    return RAI_ExecutionCtx_GetOutput(mctx->base, index);
}

void RAI_ModelRunCtxFree(RAI_ModelRunCtx *mctx) {
    RAI_ExecutionCtx_Free(mctx->base);

    RAI_Error err = {0};
    RAI_ModelFree(mctx->model, &err);

    if (err.code != RAI_OK) {
        // TODO: take it to client somehow
        RAI_ClearError(&err);
    }
    RedisModule_Free(mctx);
}

int ModelRunCtx_SetParams(RedisModuleCtx *ctx, RedisModuleString **inkeys,
                          RedisModuleString **outkeys, RAI_ModelRunCtx *mctx, RAI_Error *err) {

    RAI_Model *model = mctx->model;
    RAI_Tensor *t;
    RedisModuleKey *key;
    char *opname = NULL;
    size_t ninputs = array_len(inkeys), noutputs = array_len(outkeys);
    for (size_t i = 0; i < ninputs; i++) {
        const int status =
            RAI_GetTensorFromKeyspace(ctx, inkeys[i], &key, &t, REDISMODULE_READ, err);
        if (status == REDISMODULE_ERR) {
            return REDISMODULE_ERR;
        }
        if (model->inputs)
            opname = model->inputs[i];
        RAI_ModelRunCtxAddInput(mctx, opname, t);
    }

    for (size_t i = 0; i < noutputs; i++) {
        if (model->outputs) {
            opname = model->outputs[i];
        }
        if (!VerifyKeyInThisShard(ctx, outkeys[i])) { // Relevant for enterprise cluster.
            RAI_SetError(err, RAI_EMODELRUN,
                         "ERR CROSSSLOT Keys in request don't hash to the same slot");
            return REDISMODULE_ERR;
        }
        RAI_ModelRunCtxAddOutput(mctx, opname);
    }
    return REDISMODULE_OK;
}

int RAI_ModelRun(RAI_ModelRunCtx **mctxs, long long n, RAI_Error *err) {
    int ret;

    if (n == 0) {
        RAI_SetError(err, RAI_EBACKENDNOTLOADED, "ERR Nothing to run");
        return REDISMODULE_ERR;
    }

    RAI_ModelRunCtx **mctxs_arr = array_newlen(RAI_ModelRunCtx *, n);
    for (int i = 0; i < n; i++) {
        mctxs_arr[i] = mctxs[i];
    }

    switch (mctxs_arr[0]->model->backend) {
    case RAI_BACKEND_TENSORFLOW:
        if (!RAI_backends.tf.model_run) {
            RAI_SetError(err, RAI_EBACKENDNOTLOADED, "ERR Backend not loaded: TF");
            return REDISMODULE_ERR;
        }
        ret = RAI_backends.tf.model_run(mctxs_arr, err);
        break;
    case RAI_BACKEND_TFLITE:
        if (!RAI_backends.tflite.model_run) {
            RAI_SetError(err, RAI_EBACKENDNOTLOADED, "ERR Backend not loaded: TFLITE");
            return REDISMODULE_ERR;
        }
        ret = RAI_backends.tflite.model_run(mctxs_arr, err);
        break;
    case RAI_BACKEND_TORCH:
        if (!RAI_backends.torch.model_run) {
            RAI_SetError(err, RAI_EBACKENDNOTLOADED, "ERR Backend not loaded: TORCH");
            return REDISMODULE_ERR;
        }
        ret = RAI_backends.torch.model_run(mctxs_arr, err);
        break;
    case RAI_BACKEND_ONNXRUNTIME:
        if (!RAI_backends.onnx.model_run) {
            RAI_SetError(err, RAI_EBACKENDNOTLOADED, "ERR Backend not loaded: ONNX");
            return REDISMODULE_ERR;
        }
        ret = RAI_backends.onnx.model_run(mctxs_arr, err);
        break;
    default:
        RAI_SetError(err, RAI_EUNSUPPORTEDBACKEND, "ERR Unsupported backend");
        return REDISMODULE_ERR;
    }

    array_free(mctxs_arr);

    return ret;
}

int RAI_ModelRunAsync(RAI_ModelRunCtx *mctx, RAI_OnFinishCB ModelAsyncFinish, void *private_data) {

    RedisAI_RunInfo *rinfo = NULL;
    RAI_InitRunInfo(&rinfo);

    rinfo->single_op_dag = 1;
    rinfo->OnFinish = (RedisAI_OnFinishCB)ModelAsyncFinish;
    rinfo->private_data = private_data;

    RAI_DagOp *op;
    RAI_InitDagOp(&op);
    op->commandType = REDISAI_DAG_CMD_MODELRUN;
    op->devicestr = mctx->model->devicestr;
    op->ectx = (RAI_ExecutionCtx*)mctx;

    rinfo->dagOps = array_append(rinfo->dagOps, op);
    rinfo->dagOpCount = 1;
    if (DAG_InsertDAGToQueue(rinfo) != REDISMODULE_OK) {
        RAI_FreeRunInfo(rinfo);
        return REDISMODULE_ERR;
    }
    return REDISMODULE_OK;
}

inline RAI_Model* RAI_ModelRunCtxGetModel(RAI_ModelRunCtx* mctx) { return mctx->model;}
