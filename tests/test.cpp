#include <jnihook.h>
#include <iostream>
#include <thread>
#include <chrono>

jmethodID orig_sayHello;
void JNICALL hk_Dummy_sayHello(JNIEnv *env, jclass clazz)
{
	std::cout << "Dummy.sayHello hook called!" << std::endl;
	std::cout << "JNIEnv: " << env << std::endl;
	std::cout << "Class: " << clazz << std::endl;

	std::cout << "Original 'sayHello': " << orig_sayHello << std::endl;

	std::cout << "Calling original sayHello..." << std::endl;
	env->CallStaticVoidMethod(clazz, orig_sayHello);
	std::cout << "Called original sayHello!" << std::endl;
}

void JNICALL hk_Dummy_sayHi(JNIEnv *env, jclass clazz)
{
	std::cout << "Dummy.sayHi hook called!" << std::endl;
	std::cout << "JNIEnv: " << env << std::endl;
	std::cout << "Class: " << clazz << std::endl;
}

jclass another_clazz;
jmethodID orig_AnotherClass_getNumber;
jint JNICALL hk_AnotherClass_getNumber(JNIEnv *env, jobject obj)
{
	std::cout << "-> GETNUMBER " << std::endl;
	std::cout << "-> OBJ: " << obj << std::endl;

	std::cout << "-> Original getNumber: " << orig_AnotherClass_getNumber << std::endl;

	jint real_number = env->CallIntMethod(obj, orig_AnotherClass_getNumber);
	std::cout << "-> Actual number: " << real_number << std::endl;

	return 1337;
}

jmethodID orig_setNumber;
void JNICALL hk_AnotherClass_setNumber(JNIEnv *env, jobject obj, jint number)
{
	std::cout << "-> SETNUMBER " << std::endl;
	std::cout << "-> OBJ: " << obj << std::endl;

	std::cout << "-> Original setNumber: " << orig_setNumber << std::endl;

	std::cout << "-> Expected number: " << number << std::endl;

	env->CallVoidMethod(obj, orig_setNumber, 42);
}

void
start()
{
	JavaVM *jvm;
	JNIEnv *env;
	jsize jvm_count;
	jclass clazz;
	jmethodID sayHello_mid;
	jmethodID sayHi_mid;
	jmethodID getNumber_mid;
	jmethodID setNumber_mid;

	std::cout << "[*] Library loaded!" << std::endl;

	if (JNI_GetCreatedJavaVMs(&jvm, 1, &jvm_count) != JNI_OK) {
		std::cerr << "[!] Failed to get created Java VMs!" << std::endl;
		return;
	}

	std::cout << "[*] JavaVM: " << jvm << std::endl;

	if (jvm->AttachCurrentThread(reinterpret_cast<void **>(&env), NULL) != JNI_OK) {
		std::cerr << "[!] Failed to attach current thread to JVM!" << std::endl;
		return;
	}

	if (auto result = JNIHook_Init(jvm); result != JNIHOOK_OK) {
		std::cerr << "[!] Failed to initialize JNIHook: " << result << std::endl;
		goto DETACH;
	}

	clazz = env->FindClass("dummy/Dummy");
	std::cout << "[*] Class dummy.Dummy: " << clazz << std::endl;

	sayHello_mid = env->GetStaticMethodID(clazz, "sayHello", "()V");
	std::cout << "[*] Dummy.sayHello: " << sayHello_mid << std::endl;

	sayHi_mid = env->GetStaticMethodID(clazz, "sayHi", "()V");
	std::cout << "[*] Dummy.sayHi: " << sayHi_mid << std::endl;

	if (auto result = JNIHook_Attach(sayHello_mid, reinterpret_cast<void *>(hk_Dummy_sayHello), &orig_sayHello); result != JNIHOOK_OK) {
		std::cerr << "[!] Failed to attach hook: " << result << std::endl;
		goto DETACH;
	}

	if (auto result = JNIHook_Attach(sayHi_mid, reinterpret_cast<void *>(hk_Dummy_sayHi), nullptr); result != JNIHOOK_OK) {
		std::cerr << "[!] Failed to attach hook: " << result << std::endl;
		goto DETACH;
	}

	another_clazz = env->FindClass("dummy/AnotherClass");
	std::cout << "[*] Class dummy.AnotherClass: " << another_clazz << std::endl;

	getNumber_mid = env->GetMethodID(another_clazz, "getNumber", "()I");
	std::cout << "[*] AnotherClass.getNumber: " << getNumber_mid << std::endl;

	setNumber_mid = env->GetMethodID(another_clazz, "setNumber", "(I)V");
	std::cout << "[*] AnotherClass.setNumber: " << setNumber_mid << std::endl;

	JNIHook_Attach(getNumber_mid, reinterpret_cast<void *>(hk_AnotherClass_getNumber),
		       &orig_AnotherClass_getNumber);

	JNIHook_Attach(setNumber_mid, reinterpret_cast<void *>(hk_AnotherClass_setNumber), &orig_setNumber);

	getNumber_mid = env->GetMethodID(another_clazz, "getNumber", "()I");
	std::cout << "[*] AnotherClass.getNumber (post hook): " << getNumber_mid << std::endl;

	std::this_thread::sleep_for(std::chrono::milliseconds(2000));

	if (auto result = JNIHook_Detach(sayHi_mid); result != JNIHOOK_OK) {
		std::cout << "[*] Failed to detach hook: " << result << std::endl;
	}

	std::this_thread::sleep_for(std::chrono::milliseconds(2000));

	JNIHook_Shutdown();

	std::cout << "[*] JNIHook has been shut down" << std::endl;

DETACH:
	jvm->DetachCurrentThread(); // NOTE: The JNIEnv must live until JNIHook_Shutdown() is called!
				    //       (or if you won't call JNIHook again).
}

#ifdef _WIN32
#include <windows.h>
DWORD WINAPI WinThread(LPVOID lpParameter)
{
	start();
	return 0;
}

BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID lpReserved)
{
	switch (dwReason) {
	case DLL_PROCESS_ATTACH:
		CreateThread(nullptr, 0, WinThread, nullptr, 0, nullptr);
		break;
	}
	
	return TRUE;
}
#else
void *main_thread(void *arg)
{
	start();
	return NULL;
}

void __attribute__((constructor))
dl_entry()
{
	pthread_t th;
	pthread_create(&th, NULL, main_thread, NULL);
}
#endif
