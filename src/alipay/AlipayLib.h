#pragma once

#include <string>

extern "C" {
    char* GenerateTransferScheme(char* userID, char* amount, char* memo);
    char* GenerateAddFriendScheme(char* userID, char* loginID);
    char* GenerateRedPacketScheme(char* loginID, char* userID, char* userName, char* amount, char* remark);
    void FreeString(char* s);
}

class AlipayLib {
public:
    // 生成转账 Scheme
    static std::string generateTransferScheme(
        const std::string& userID,
        const std::string& amount,
        const std::string& memo
    ) {
        char* result = GenerateTransferScheme(
            const_cast<char*>(userID.c_str()),
            const_cast<char*>(amount.c_str()),
            const_cast<char*>(memo.c_str())
        );
        std::string scheme(result);
        FreeString(result);
        return scheme;
    }

    // 生成添加好友 Scheme
    static std::string generateAddFriendScheme(
        const std::string& userID,
        const std::string& loginID
    ) {
        char* result = GenerateAddFriendScheme(
            const_cast<char*>(userID.c_str()),
            const_cast<char*>(loginID.c_str())
        );
        std::string scheme(result);
        FreeString(result);
        return scheme;
    }

    // 生成红包 Scheme
    static std::string generateRedPacketScheme(
        const std::string& loginID,
        const std::string& userID,
        const std::string& userName,
        const std::string& amount,
        const std::string& remark
    ) {
        char* result = GenerateRedPacketScheme(
            const_cast<char*>(loginID.c_str()),
            const_cast<char*>(userID.c_str()),
            const_cast<char*>(userName.c_str()),
            const_cast<char*>(amount.c_str()),
            const_cast<char*>(remark.c_str())
        );
        std::string scheme(result);
        FreeString(result);
        return scheme;
    }
};
