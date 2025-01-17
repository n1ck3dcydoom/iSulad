/******************************************************************************
 * Copyright (c) Huawei Technologies Co., Ltd. 2019. All rights reserved.
 * iSulad licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *     http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
 * PURPOSE.
 * See the Mulan PSL v2 for more details.
 * Author: wujing
 * Create: 2019-05-17
 * Description: provide c++ common utils functions
 *******************************************************************************/
#include "cxxutils.h"
#include <algorithm>
#include <numeric>

namespace CXXUtils {
std::vector<std::string> Split(const std::string &str, char delimiter)
{
    std::vector<std::string> retVec;
    std::string tmpStr;
    std::istringstream istream(str);
    while (std::getline(istream, tmpStr, delimiter)) {
        retVec.push_back(tmpStr);
    }
    return retVec;
}

// more than one contiguous delimiter is considered as a single delimiter
std::vector<std::string> SplitDropEmpty(const std::string &str, char delimiter)
{
    std::vector<std::string> retVec;
    std::string tmpStr;
    std::istringstream istream(str);
    while (std::getline(istream, tmpStr, delimiter)) {
        if (tmpStr.empty()) {
            continue;
        }
        retVec.push_back(tmpStr);
    }
    return retVec;
}

// Join concatenates the elements of a to create a single string. The separator string
// sep is placed between elements in the resulting string.
std::string StringsJoin(const std::vector<std::string> &vec, const std::string &sep)
{
    auto func = [&sep](const std::string & a, const std::string & b) -> std::string {
        return a + (a.length() > 0 ? sep : "") + b;
    };
    return std::accumulate(vec.begin(), vec.end(), std::string(), func);
}

std::string StringTrim(const std::string &str)
{
    auto pos1 = str.find_first_not_of(' ');
    if (pos1 == std::string::npos) {
        return str;
    }

    auto pos2 = str.find_last_not_of(' ');
    if (pos2 == std::string::npos) {
        return str.substr(pos1);
    }

    return str.substr(pos1, pos2 - pos1 + 1);
}

} // namespace CXXUtils

