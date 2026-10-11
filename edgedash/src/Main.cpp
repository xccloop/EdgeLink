#include "entry/EasyUIContext.h"
#include "uart/UartContext.h"
#include "manager/ConfigManager.h"
#include "net/NetManager.h"
#include "utils/Log.h"
#include <cstring>
#include <string>

namespace {

// 与 edgenode/User/App/Config/config.c 中的热点配置保持一致。
const char WIFI_SSID[] = "ChinaNet-pehm";
const char WIFI_PASSWORD[] = "yrdhziq9";

class StartupWifiThread : public Thread {
public:
    StartupWifiThread() : mWifi(NULL), mScanRequested(false), mConnectRequested(false), mPollCount(0) { }

protected:
    virtual bool readyToRun() {
        mScanRequested = false;
        mConnectRequested = false;
        mPollCount = 0;
        mLastIp.clear();
        mWifi = NETMANAGER->getWifiManager();
        if (mWifi == NULL || !mWifi->isSupported()) {
            LOGE("Startup WiFi: device does not support WiFi");
            return false;
        }
        if (!mWifi->isWifiEnable()) {
            mWifi->enableWifi(true);
        }
        LOGD("Startup WiFi: waiting for WiFi to enable");
        return true;
    }

    virtual bool threadLoop() {
        if (exitPending()) {
            return false;
        }
        if (++mPollCount > 300) {
            if (!mConnectRequested) {
                LOGE("Startup WiFi: target AP not found or WiFi not ready after 60 seconds");
            } else if (mLastIp.empty()) {
                LOGE("Startup WiFi: connection/IP timeout after 60 seconds");
            } else {
                LOGD("Startup WiFi: startup IP monitoring finished");
            }
            return false;
        }

        if (mWifi->isWifiEnable()) {
            if (!mConnectRequested) {
                if (!mScanRequested || mPollCount % 25 == 0) {
                    mWifi->scan();
                    mScanRequested = true;
                    LOGD("Startup WiFi: scanning for target AP");
                }
                // 双参数 connect 在扫描列表中找不到 SSID 时会直接返回。
                std::vector<WifiInfo> accessPoints;
                mWifi->getWifiScanInfosLock(accessPoints);
                for (const WifiInfo &ap : accessPoints) {
                    if (ap.getSsid() == WIFI_SSID) {
                        mWifi->connect(WIFI_SSID, WIFI_PASSWORD, ap.getEncryption());
                        mConnectRequested = true;
                        LOGD("Startup WiFi: target AP found, connection requested");
                        break;
                    }
                }
            } else if (mWifi->isConnected()) {
                const WifiInfo *info = mWifi->getConnectionInfo();
                const char *ip = mWifi->getIp();
                if (info != NULL && info->getSsid() == WIFI_SSID &&
                    ip != NULL && ip[0] != '\0' && std::strcmp(ip, "0.0.0.0") != 0) {
                    // 认证完成时接口可能仍保留旧 IP，继续观察 DHCP 后续更新。
                    if (mLastIp != ip) {
                        mLastIp = ip;
                        LOGD("Startup WiFi: current IP=%s (DHCP may update it)", mLastIp.c_str());
                    }
                }
            }
        }
        Thread::sleep(200);
        return true;
    }

private:
    WifiManager *mWifi;
    bool mScanRequested;
    bool mConnectRequested;
    int mPollCount;
    std::string mLastIp;
};

StartupWifiThread startupWifiThread;

} // namespace

#ifdef __cplusplus
extern "C" {
#endif  /* __cplusplus */

void onEasyUIInit(EasyUIContext *pContext) {
	// 初始化时打开串口
	UARTCONTEXT->openUart(CONFIGMANAGER->getUartName().c_str(), CONFIGMANAGER->getUartBaudRate());
    if (!startupWifiThread.run("startup_wifi")) {
        LOGE("Startup WiFi: failed to start thread");
    }
}

void onEasyUIDeinit(EasyUIContext *pContext) {
    startupWifiThread.requestExitAndWait();
	UARTCONTEXT->closeUart();
}

const char* onStartupApp(EasyUIContext *pContext) {
	return "mainActivity";
}


#ifdef __cplusplus

}

#endif  /* __cplusplus */

