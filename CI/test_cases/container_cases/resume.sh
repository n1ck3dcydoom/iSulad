#!/bin/bash
#
# attributes: isulad inheritance resume
# concurrent: YES
# spend time: 1

#######################################################################
##- Copyright (c) Huawei Technologies Co., Ltd. 2020. All rights reserved.
# - iSulad licensed under the Mulan PSL v2.
# - You can use this software according to the terms and conditions of the Mulan PSL v2.
# - You may obtain a copy of Mulan PSL v2 at:
# -     http://license.coscl.org.cn/MulanPSL2
# - THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
# - IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
# - PURPOSE.
# - See the Mulan PSL v2 for more details.
##- @Description:CI
##- @Author: lifeng
##- @Create: 2020-03-30
#######################################################################

curr_path=$(dirname $(readlink -f "$0"))
data_path=$(realpath $curr_path/../data)
source ../helpers.sh

function do_test_t()
{
    local runtime=$1
    local test="kill_test => (${runtime})"
    msg_info "${test} starting..."

    containername=test_resume
    isula create -t --name $containername --runtime $runtime busybox
    fn_check_eq "$?" "0" "create failed"
    testcontainer $containername inited

    isula unpause $containername
    fn_check_ne "$?" "0" "resume should fail"
    testcontainer $containername inited

    isula start $containername
    fn_check_eq "$?" "0" "start failed"
    testcontainer $containername running

    isula pause $containername
    fn_check_eq "$?" "0" "pause failed"

    testcontainer $containername paused

    isula unpause $containername
    fn_check_eq "$?" "0" "resume failed"
    testcontainer $containername running

    isula rm -f $containername
    fn_check_eq "$?" "0" "rm failed"

    msg_info "${test} finished with return ${TC_RET_T}..."

    return $TC_RET_T
}

ret=0

for element in ${RUNTIME_LIST[@]};
do
    do_test_t $element
    if [ $? -ne 0 ];then
        let "ret=$ret + 1"
    fi
done

show_result $ret "basic resume"
