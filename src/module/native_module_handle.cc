#include "native_module_handle.h"
#include "context_handle.h"
#include "reference_handle.h"
#include "isolate/environment.h"
#include "isolate/remote_handle.h"
#include "isolate/three_phase_task.h"

using namespace v8;
using std::shared_ptr;
using std::unique_ptr;

namespace ivm {

/**
 * RAII wrapper around libuv dlopen
 */
NativeModule::NativeModule(const std::string& filename) : init(nullptr) {
	if (uv_dlopen(filename.c_str(), &lib) != 0) {
		throw RuntimeGenericError("Failed to load module");
	}
	if (uv_dlsym(&lib, "InitForContext", reinterpret_cast<void**>(&init)) != 0 || init == nullptr) {
		uv_dlclose(&lib);
		throw RuntimeGenericError("Module is not isolated-vm compatible");
	}
}

NativeModule::~NativeModule() {
	uv_dlclose(&lib);
}

void NativeModule::InitForContext(Isolate* isolate, Local<Context> context, Local<Object> target) {
	init(isolate, context, target);
}

/**
 * Simple transferable logic so we can transfer native module handle between isolates
 */
NativeModuleHandle::NativeModuleTransferable::NativeModuleTransferable(shared_ptr<NativeModule> module) : module(std::move(module)) {}

Local<Value> NativeModuleHandle::NativeModuleTransferable::TransferIn() {
	return ClassHandle::NewInstance<NativeModuleHandle>(module);
}

/**
 * Native module JS API
 */
NativeModuleHandle::NativeModuleHandle(shared_ptr<NativeModule> module) : module(std::move(module)) {}

Local<FunctionTemplate> NativeModuleHandle::Definition() {
	return Inherit<TransferableHandle>(MakeClass(
		"NativeModule", ConstructorFunction<decltype(&New), &New>{},
		"create", MemberFunction<decltype(&NativeModuleHandle::Create<1>), &NativeModuleHandle::Create<1>>{},
		"createSync", MemberFunction<decltype(&NativeModuleHandle::Create<0>), &NativeModuleHandle::Create<0>>{}
	));
}

unique_ptr<NativeModuleHandle> NativeModuleHandle::New(Local<String> value) {
	return std::make_unique<NativeModuleHandle>(
		std::make_shared<NativeModule>(*String::Utf8Value{Isolate::GetCurrent(), value})
	);
}

unique_ptr<Transferable> NativeModuleHandle::TransferOut() {
	return std::make_unique<NativeModuleTransferable>(module);
}

class CreateRunner : public ThreePhaseTask {
	private:
		RemoteHandle<Context> context;
		shared_ptr<NativeModule> module;
		unique_ptr<Transferable> result;

	public:
		CreateRunner(RemoteHandle<Context> context, shared_ptr<NativeModule> module) : context(std::move(context)), module(std::move(module)) {}

	protected:
		void Phase2() final {
			Isolate* isolate = Isolate::GetCurrent();
			Local<Context> context_handle = Deref(context);
			Context::Scope context_scope(context_handle);
			Local<Object> exports = Object::New(isolate);
			module->InitForContext(isolate, context_handle, exports);
			// Once a native module is imported into an isolate, that isolate holds a reference to the module forever
			auto ptr = module.get();
			Executor::GetCurrentEnvironment()->native_modules.emplace(ptr, std::move(module));
			result = std::make_unique<ReferenceHandleTransferable>(exports);
		}

		Local<Value> Phase3() final {
			return result->TransferIn();
		}
};
template <int async>
Local<Value> NativeModuleHandle::Create(class ContextHandle& context_handle) {
	// TODO: This should probably throw from the promise, but ThreePhaseTask can't handle invalid
	// isolate references for now.
	auto context = context_handle.GetContext();
	return ThreePhaseTask::Run<async, CreateRunner>(*context.GetIsolateHolder(), context, module);
}

} // namespace ivm
