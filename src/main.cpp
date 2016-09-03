#include <nan.h>
#include <cassert>
#include <iostream>

namespace flacseektable {

using node::AtExit;

using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::Isolate;
using v8::Local;
using v8::Object;
using v8::String;
using v8::Value;

using Nan::GetFunction;
using Nan::New;
using Nan::Set;

void shutdown(void*) {
	std::cout << "[+] shutdown" << std::endl;
}

NAN_MODULE_INIT(initialize) {
	AtExit(shutdown);
	std::cout << "[+] init" << std::endl;
}

NODE_MODULE(flacseektable, initialize)

}
