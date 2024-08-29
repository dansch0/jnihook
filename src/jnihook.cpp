/*
 *  -----------------------------------
 * |         JNIHook - by rdbo         |
 * |      Java VM Hooking Library      |
 *  -----------------------------------
 */

/*
 * Copyright (C) 2023    Rdbo
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License version 3
 * as published by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 * 
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <jnihook.h>
#include <unordered_map>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <tuple>
#include "classfile.hpp"
#include "uuid.hpp"

typedef struct jnihook_t {
	JavaVM   *jvm;
	jvmtiEnv *jvmti;
} jnihook_t;

typedef struct method_info_t {
	std::string name;
	std::string signature;
	jint access_flags;
} method_info_t;

typedef struct hook_info_t {
	method_info_t method_info;
	void *native_hook_method;
} hook_info_t;

static std::unique_ptr<jnihook_t> g_jnihook = nullptr;
static std::unordered_map<std::string, std::vector<hook_info_t>> g_hooks;
static std::unordered_map<std::string, std::unique_ptr<ClassFile>> g_class_file_cache;
// Tuple order: class name, method name, method signature
static std::unordered_map<std::string, std::unique_ptr<void *>> g_original_methods = {};

static std::string
get_class_signature(jvmtiEnv *jvmti, jclass clazz)
{
	char *sig;
	
	if (jvmti->GetClassSignature(clazz, &sig, NULL) != JVMTI_ERROR_NONE) {
		return "";
	}

	std::string signature = std::string(sig, &sig[strlen(sig)]);

	jvmti->Deallocate(reinterpret_cast<unsigned char *>(sig));

	return signature;
}

static std::string
get_class_name(JNIEnv *env, jclass clazz)
{
	jclass klass = env->FindClass("java/lang/Class");
	if (!klass)
		return "";

	jmethodID getName_method = env->GetMethodID(klass, "getName", "()Ljava/lang/String;");
	if (!getName_method)
		return "";

	jstring name_obj = reinterpret_cast<jstring>(env->CallObjectMethod(clazz, getName_method));
	if (!name_obj)
		return "";

	const char *c_name = env->GetStringUTFChars(name_obj, 0);
	if (!c_name)
		return "";

	std::string name = std::string(c_name, &c_name[strlen(c_name)]);

	env->ReleaseStringUTFChars(name_obj, c_name);

	// Replace dots with slashes to match contents of ClassFile
	for (size_t i = 0; i < name.length(); ++i) {
		if (name[i] == '.')
			name[i] = '/';
	}
	
	return name;
}

static std::unique_ptr<method_info_t>
get_method_info(jvmtiEnv *jvmti, jmethodID method)
{
	char *name;
	char *sig;
	jint access_flags;
	
	if (jvmti->GetMethodName(method, &name, &sig, NULL) != JVMTI_ERROR_NONE)
		return nullptr;

	if (jvmti->GetMethodModifiers(method, &access_flags) != JVMTI_ERROR_NONE)
		return nullptr;

	std::string name_str(name, &name[strlen(name)]);
	std::string signature_str(sig, &sig[strlen(sig)]);

	jvmti->Deallocate(reinterpret_cast<unsigned char *>(name));
	jvmti->Deallocate(reinterpret_cast<unsigned char *>(sig));

	return std::make_unique<method_info_t>(method_info_t { name_str, signature_str, access_flags });
}

static std::string
get_method_lookup_key(std::string class_name, std::string method_name, std::string method_signature)
{
	return class_name + "_" + method_name + "_" + method_signature;
}

void JNICALL JNIHook_ClassFileLoadHook(jvmtiEnv *jvmti_env,
				       JNIEnv* jni_env,
				       jclass class_being_redefined,
				       jobject loader,
				       const char* name,
				       jobject protection_domain,
				       jint class_data_len,
				       const unsigned char* class_data,
				       jint* new_class_data_len,
				       unsigned char** new_class_data)
{
	auto class_name = get_class_name(jni_env, class_being_redefined);

	// Don't do anything for unhooked classes
	if (class_name == "" || g_hooks.find(class_name) == g_hooks.end() || g_hooks[class_name].size() == 0)
		return;

	// Cache parsed ClassFile if it's not cached yet
	if (g_class_file_cache.find(class_name) == g_class_file_cache.end()) {
		auto cf = ClassFile::load(class_data);
		if (!cf)
			return;

		g_class_file_cache[class_name] = std::move(cf);
	}

	return;
}

// Patches up a class with the current hooks (if any)
// and redefines it using JVMTI
jnihook_result_t
ReapplyClass(jclass clazz, std::string clazz_name)
{
	jvmtiClassDefinition class_definition;

	auto cf = *g_class_file_cache[clazz_name];

	auto constant_pool = cf.get_constant_pool();

	// Patch class file
	// NOTE: The `methods` attribute only has the methods defined by the main class of this ClassFile
	//       Method references are not included here
	//       If the source file has more than one class, they are compiled as separate ClassFiles
	for (auto &method : cf.get_methods()) {
		auto name_ci = reinterpret_cast<CONSTANT_Utf8_info *>(
			cf.get_constant_pool_item(method.name_index).bytes.data()
		);

		auto descriptor_ci = reinterpret_cast<CONSTANT_Utf8_info *>(
			cf.get_constant_pool_item(method.descriptor_index).bytes.data()
		);

		auto name = std::string(name_ci->bytes, &name_ci->bytes[name_ci->length]);
		auto descriptor = std::string(descriptor_ci->bytes, &descriptor_ci->bytes[descriptor_ci->length]);

		// Check if the current method is a method that should be hooked
		// TODO: Use hashmap for faster lookup
		bool should_hook = false;
		for (auto &hk_info : g_hooks[clazz_name]) {
			auto &minfo = hk_info.method_info;
			if (minfo.name == name && minfo.signature == descriptor) {
				should_hook = true;
				break;
			}
		}
		if (!should_hook)
			continue;

		// Set method to native
		method.access_flags |= ACC_NATIVE;

		// Remove "Code" attribute
		for (size_t i = 0; i < method.attributes.size(); ++i) {
			auto attr = method.attributes[i];
			auto attr_name_ci = reinterpret_cast<CONSTANT_Utf8_info *>(
				cf.get_constant_pool_item(attr.attribute_name_index).bytes.data()
			);
			auto attr_name = std::string(attr_name_ci->bytes, &attr_name_ci->bytes[attr_name_ci->length]);
			if (attr_name == "Code") {
				method.attributes.erase(method.attributes.begin() + i);
				break;
			}
		}
	}

	// Redefine class with modified ClassFile
	auto cf_bytes = cf.bytes();

	class_definition.klass = clazz;
	class_definition.class_byte_count = cf_bytes.size();
	class_definition.class_bytes = cf_bytes.data();
	if (g_jnihook->jvmti->RedefineClasses(1, &class_definition) != JVMTI_ERROR_NONE)
		return JNIHOOK_ERR_JVMTI_OPERATION;

	return JNIHOOK_OK;
}

JNIHOOK_API jnihook_result_t JNIHOOK_CALL
JNIHook_Init(JavaVM *jvm)
{
	jvmtiEnv *jvmti;
	jvmtiCapabilities capabilities;
	jvmtiEventCallbacks callbacks = {};

	if (jvm->GetEnv(reinterpret_cast<void **>(&jvmti), JVMTI_VERSION_1_2) != JNI_OK) {
		return JNIHOOK_ERR_GET_JVMTI;
	}

	if (jvmti->GetPotentialCapabilities(&capabilities) != JVMTI_ERROR_NONE) {
		return JNIHOOK_ERR_ADD_JVMTI_CAPS;
	}

	capabilities.can_redefine_classes = 1;
	capabilities.can_redefine_any_class = 1;
	capabilities.can_retransform_classes = 1;
	capabilities.can_retransform_any_class = 1;
	capabilities.can_suspend = 1;

	if (jvmti->AddCapabilities(&capabilities) != JVMTI_ERROR_NONE) {
		return JNIHOOK_ERR_ADD_JVMTI_CAPS;
	}

	callbacks.ClassFileLoadHook = JNIHook_ClassFileLoadHook;
	if (jvmti->SetEventCallbacks(&callbacks, sizeof(callbacks)) != JVMTI_ERROR_NONE) {
		return JNIHOOK_ERR_SETUP_CLASS_FILE_LOAD_HOOK;
	}

	g_jnihook = std::make_unique<jnihook_t>(jnihook_t { jvm, jvmti });

	return JNIHOOK_OK;
}

JNIHOOK_API jnihook_result_t JNIHOOK_CALL
JNIHook_Attach(jmethodID method, void *native_hook_method, jmethodID *original_method)
{
	jclass clazz;
	std::string clazz_name;
	hook_info_t hook_info;
	jobject class_loader;
	JNIEnv *env;
	// jmethodID is a double pointer. its inner pointer is the real method.
	void *orig_method_deref = *reinterpret_cast<void **>(method); 

	if (g_jnihook->jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_8)) {
		return JNIHOOK_ERR_GET_JNI;
	}

	if (g_jnihook->jvmti->GetMethodDeclaringClass(method, &clazz) != JVMTI_ERROR_NONE) {
		return JNIHOOK_ERR_JVMTI_OPERATION;
	}

	clazz_name = get_class_name(env, clazz);
	if (clazz_name.length() == 0) {
		return JNIHOOK_ERR_JNI_OPERATION;
	}

	auto method_info = get_method_info(g_jnihook->jvmti, method);
	if (!method_info) {
		return JNIHOOK_ERR_JVMTI_OPERATION;
	}

	hook_info.method_info = *method_info;
	hook_info.native_hook_method = native_hook_method;

	// Force caching of the class being hooked
	if (g_class_file_cache.find(clazz_name) == g_class_file_cache.end()) {
		if (g_jnihook->jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, NULL) != JVMTI_ERROR_NONE) {
			return JNIHOOK_ERR_SETUP_CLASS_FILE_LOAD_HOOK;
		}

		// Temporarily register hook in g_hooks so that `ClassFileLoadHook` can see it
		// Leaving it there could be a problem if this hook fails, it will still patch
		// the class when JNIHook_Attach is called again for that same class, but won't
		// register the native method, causing `java.lang.UnsatisfiedLinkError`.
		g_hooks[clazz_name].push_back(hook_info);
		auto result = g_jnihook->jvmti->RetransformClasses(1, &clazz);
		g_hooks[clazz_name].pop_back();

		// NOTE: We disable the ClassFileLoadHook here because it breaks
		//       any `env->DefineClass()` calls. Also, it's not necessary
		//       to keep it active at all times, we just have to use it for caching
		//       uncached hooked classes.
		// TODO: Investigate why it breaks it (possibly NullPointerException in
		//       JNIHook_ClassFileLoadHook)
		if (g_jnihook->jvmti->SetEventNotificationMode(JVMTI_DISABLE, JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, NULL) != JVMTI_ERROR_NONE) {
			return JNIHOOK_ERR_SETUP_CLASS_FILE_LOAD_HOOK;
		}

		if (result != JVMTI_ERROR_NONE)
			return JNIHOOK_ERR_CLASS_FILE_CACHE;

		if (g_class_file_cache.find(clazz_name) == g_class_file_cache.end()) {
			return JNIHOOK_ERR_CLASS_FILE_CACHE;
		}
	}

	// Store original method dereference in a new allocation
	std::string method_lookup_key = get_method_lookup_key(clazz_name, method_info->name, method_info->signature);
	g_original_methods[method_lookup_key] = std::make_unique<void *>(orig_method_deref);

	// Suspend other threads while the hook is being set up
	jthread curthread;
	jthread *threads;
	jint thread_count;

	env->PushLocalFrame(16);
	
	if (g_jnihook->jvmti->GetCurrentThread(&curthread) != JVMTI_ERROR_NONE)
		return JNIHOOK_ERR_JVMTI_OPERATION;

	if (g_jnihook->jvmti->GetAllThreads(&thread_count, &threads) != JVMTI_ERROR_NONE)
		return JNIHOOK_ERR_JVMTI_OPERATION;

	// TODO: Only suspend/resume threads that are actually active
	for (jint i = 0; i < thread_count; ++i) {
		if (env->IsSameObject(threads[i], curthread))
			continue;

		g_jnihook->jvmti->SuspendThread(threads[i]);
	}

	// Apply current hooks
	jnihook_result_t ret;
	g_hooks[clazz_name].push_back(hook_info);
	if (ret = ReapplyClass(clazz, clazz_name); ret != JNIHOOK_OK) {
		g_hooks[clazz_name].pop_back();
		goto RESUME_THREADS;
	}

	// Register native method for JVM lookup
	JNINativeMethod native_method;
	native_method.name = const_cast<char *>(method_info->name.c_str());
	native_method.signature = const_cast<char *>(method_info->signature.c_str());
	native_method.fnPtr = native_hook_method;

	if (env->RegisterNatives(clazz, &native_method, 1) < 0) {
		g_hooks[clazz_name].pop_back();
		ReapplyClass(clazz, clazz_name); // Attempt to restore class to previous state
		ret = JNIHOOK_ERR_JNI_OPERATION;
		goto RESUME_THREADS;
	}

	// Save the pointer to the original method dereference in 'original_method'
	if (original_method) {
		*original_method = reinterpret_cast<jmethodID>(g_original_methods[method_lookup_key].get());
	}

	ret = JNIHOOK_OK;

RESUME_THREADS:
	// Resume other threads, hook already placed succesfully
	for (jint i = 0; i < thread_count; ++i) {
		if (env->IsSameObject(threads[i], curthread))
			continue;

		g_jnihook->jvmti->ResumeThread(threads[i]);
	}

	g_jnihook->jvmti->Deallocate(reinterpret_cast<unsigned char *>(threads));
	env->PopLocalFrame(NULL);

	return ret;
}

JNIHOOK_API jnihook_result_t JNIHOOK_CALL
JNIHook_Detach(jmethodID method)
{
	JNIEnv *env;
	jclass clazz;
	std::string clazz_name;
	hook_info_t hook_info;
	jvmtiClassDefinition class_definition;

	if (g_jnihook->jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_8)) {
		return JNIHOOK_ERR_GET_JNI;
	}

	if (g_jnihook->jvmti->GetMethodDeclaringClass(method, &clazz) != JVMTI_ERROR_NONE) {
		return JNIHOOK_ERR_JVMTI_OPERATION;
	}

	clazz_name = get_class_name(env, clazz);
	if (clazz_name.length() == 0) {
		return JNIHOOK_ERR_JNI_OPERATION;
	}

	if (g_hooks.find(clazz_name) == g_hooks.end() || g_hooks[clazz_name].size() == 0) {
		return JNIHOOK_OK;
	}

	auto method_info = get_method_info(g_jnihook->jvmti, method);
	if (!method_info) {
		return JNIHOOK_ERR_JVMTI_OPERATION;
	}

	for (size_t i = 0; i < g_hooks[clazz_name].size(); ++i) {
		auto &hook_info = g_hooks[clazz_name][i];
		if (hook_info.method_info.name != method_info->name ||
		    hook_info.method_info.signature != method_info->signature)
			continue;

		g_hooks[clazz_name].erase(g_hooks[clazz_name].begin() + i);
		g_original_methods.erase(get_method_lookup_key(clazz_name, method_info->name, method_info->signature));
		break;
	}

	return ReapplyClass(clazz, clazz_name);
}


JNIHOOK_API jnihook_result_t JNIHOOK_CALL
JNIHook_Shutdown()
{
	JNIEnv *env;
	jvmtiEventCallbacks callbacks = {};

	if (g_jnihook->jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_8)) {
		return JNIHOOK_ERR_GET_JNI;
	}

	for (auto &[key, _value] : g_class_file_cache) {
		jclass clazz = env->FindClass(key.c_str());

		g_hooks[key].clear();

		if (!clazz)
			continue;

		// Reapplying the class with empty hooks will just restore the original one.
		ReapplyClass(clazz, key);
	}

	g_class_file_cache.clear();

	g_original_methods.clear();

	g_jnihook->jvmti->SetEventNotificationMode(JVMTI_DISABLE, JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, NULL);
	g_jnihook->jvmti->SetEventCallbacks(&callbacks, sizeof(callbacks));

	g_jnihook = nullptr;

	return JNIHOOK_OK;
}
