// Copyright (c) 2017 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/renderer/web_worker_observer.h"

#include "base/lazy_instance.h"
#include "base/threading/thread_local.h"
#include "shell/common/api/electron_bindings.h"
#include "shell/common/gin_helper/event_emitter_caller.h"
#include "shell/common/node_bindings.h"
#include "shell/common/node_includes.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"

namespace electron {

namespace {

static base::LazyInstance<
    base::ThreadLocalPointer<WebWorkerObserver>>::DestructorAtExit lazy_tls =
    LAZY_INSTANCE_INITIALIZER;

}  // namespace

// static
WebWorkerObserver* WebWorkerObserver::GetCurrent() {
  WebWorkerObserver* self = lazy_tls.Pointer()->Get();
  return self ? self : new WebWorkerObserver;
}

WebWorkerObserver::WebWorkerObserver()
    : node_bindings_(
          NodeBindings::Create(NodeBindings::BrowserEnvironment::kWorker)),
      electron_bindings_(
          std::make_unique<ElectronBindings>(node_bindings_->uv_loop())) {
  lazy_tls.Pointer()->Set(this);
}

WebWorkerObserver::~WebWorkerObserver() {
  lazy_tls.Pointer()->Set(nullptr);

  if (!has_node_integration_)
    return;

  // Destroying the node environment will also run the uv loop,
  // Node.js expects `kExplicit` microtasks policy and will run microtasks
  // checkpoints after every call into JavaScript. Since we use a different
  // policy in the renderer - switch to `kExplicit`
  v8::Isolate* isolate = node_bindings_->uv_env()->isolate();
  DCHECK_EQ(v8::MicrotasksScope::GetCurrentDepth(isolate), 0);
  isolate->SetMicrotasksPolicy(v8::MicrotasksPolicy::kExplicit);
  node::FreeEnvironment(node_bindings_->uv_env());
  node::FreeIsolateData(node_bindings_->isolate_data());
}

void WebWorkerObserver::WorkerScriptReadyForEvaluation(
    v8::Local<v8::Context> worker_context) {
  auto* execution_context = blink::ExecutionContext::From(worker_context);

  // We do not create a Node.js environment in service or shared workers
  // owing to an inability to customize sandbox policies in these workers
  // given that they're run out-of-process.
  bool is_service_worker = execution_context->IsServiceWorkerGlobalScope();
  bool is_shared_worker = execution_context->IsSharedWorkerGlobalScope();
  if (is_service_worker || is_shared_worker) {
    has_node_integration_ = false;
    return;
  }

  v8::Context::Scope context_scope(worker_context);
  auto* isolate = worker_context->GetIsolate();
  v8::MicrotasksScope microtasks_scope(
      isolate, v8::MicrotasksScope::kDoNotRunMicrotasks);

  // Start the embed thread.
  node_bindings_->PrepareEmbedThread();

  // Setup node tracing controller.
  if (!node::tracing::TraceEventHelper::GetAgent())
    node::tracing::TraceEventHelper::SetAgent(node::CreateAgent());

  // Setup node environment for each window.
  bool initialized = node::InitializeContext(worker_context);
  CHECK(initialized);
  node::Environment* env =
      node_bindings_->CreateEnvironment(worker_context, nullptr);

  // Add Electron extended APIs, wrap uv loop, and begin polling.
  electron_bindings_->BindTo(env->isolate(), env->process_object());
  node_bindings_->LoadEnvironment(env);
  node_bindings_->set_uv_env(env);
  node_bindings_->StartPolling();
}

void WebWorkerObserver::ContextWillDestroy(v8::Local<v8::Context> context) {
  node::Environment* env = node::Environment::GetCurrent(context);
  if (env)
    gin_helper::EmitEvent(env->isolate(), env->process_object(), "exit");

  delete this;
}

}  // namespace electron
