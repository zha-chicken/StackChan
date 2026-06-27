#pragma once

#include <string>

namespace haolab {

class UserAvatarSync {
public:
    static void SyncFromUrl(const std::string& url);
};

}  // namespace haolab
