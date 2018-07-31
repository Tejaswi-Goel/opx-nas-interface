/*
 * Copyright (c) 2018 Dell Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 *
 * THIS CODE IS PROVIDED ON AN *AS IS* BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT
 * LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS
 * FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.
 *
 * See the Apache Version 2.0 License for specific language governing
 * permissions and limitations under the License.
 */

/*
 * filename: nas_stats_vlan_subintf.cpp
 */



#include "dell-base-if.h"
#include "dell-interface.h"
#include "ietf-interfaces.h"
#include "interface/nas_interface_map.h"
#include "hal_if_mapping.h"
#include "interface/nas_interface_vlan.h"

#include "cps_api_key.h"
#include "cps_api_object_key.h"
#include "cps_class_map.h"
#include "cps_api_operation.h"
#include "event_log.h"
#include "nas_ndi_plat_stat.h"
#include "nas_stats.h"
#include "nas_ndi_vlan.h"
#include "ds_common_types.h"
#include "nas_switch.h"
#include "interface_obj.h"
#include "nas_ndi_1d_bridge.h"

#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <stdint.h>
#include <time.h>

static const auto vlan_subintf_stat_ids = new std::vector<ndi_stat_id_t>  {
        DELL_IF_IF_INTERFACES_STATE_INTERFACE_STATISTICS_IN_PKTS,
        DELL_IF_IF_INTERFACES_STATE_INTERFACE_STATISTICS_OUT_PKTS,
        IF_INTERFACES_STATE_INTERFACE_STATISTICS_IN_OCTETS,
        IF_INTERFACES_STATE_INTERFACE_STATISTICS_OUT_OCTETS
};

static bool _fill_vlan_sub_intf_info(cps_api_object_t obj, NAS_VLAN_INTERFACE * &vlan_obj,
                                     interface_ctrl_t & intf_ctrl){

    cps_api_object_attr_t _if_name_attr  = cps_api_get_key_data(obj,IF_INTERFACES_STATE_INTERFACE_NAME);
    if(!_if_name_attr){
        _if_name_attr = cps_api_get_key_data(obj,DELL_IF_CLEAR_COUNTERS_INPUT_INTF_CHOICE_IFNAME_CASE_IFNAME);
    }

    if(!_if_name_attr){
        EV_LOGGING(INTERFACE,ERR,"NAS-STAT","No Interface name passed to get vlan sub interface stat");
        return false;
    }

    const char * _if_name = (const char *)cps_api_object_attr_data_bin(_if_name_attr);
    std::string vlan_name = std::string(_if_name);

    vlan_obj = dynamic_cast<NAS_VLAN_INTERFACE *>(nas_interface_map_obj_get(vlan_name));
    if(!vlan_obj){
        EV_LOGGING(INTERFACE,ERR,"NAS-VLAN-SUB-INTF-STAT","No object for vlan sub intf %s exist",_if_name);
        return false;
    }

    memset(&intf_ctrl, 0, sizeof(interface_ctrl_t));
    intf_ctrl.q_type = HAL_INTF_INFO_FROM_IF_NAME;
    memcpy(intf_ctrl.if_name,vlan_obj->parent_intf_name.c_str(),sizeof(intf_ctrl.if_name));

    if (dn_hal_get_interface_info(&intf_ctrl) != STD_ERR_OK) {
       EV_LOGGING(INTERFACE,ERR,"NAS-STAT","Failed to find interface information for bridge %s",_if_name);
       return false;
    }

    return true;
}

static cps_api_return_code_t if_stats_get (void * context, cps_api_get_params_t * param,
                                           size_t ix) {

    cps_api_object_t obj = cps_api_object_list_get(param->filters,ix);
    NAS_VLAN_INTERFACE *vlan_obj=nullptr;
    interface_ctrl_t intf_ctrl;

    if(!_fill_vlan_sub_intf_info(obj,vlan_obj,intf_ctrl)){
        return cps_api_ret_code_ERR;
    }

    uint64_t stat_val[vlan_subintf_stat_ids->size()];

    if ((intf_ctrl.int_type == nas_int_type_PORT)){
        if(ndi_bridge_port_stats_get(intf_ctrl.npu_id,intf_ctrl.port_id,vlan_obj->vlan_id,
                                     (ndi_stat_id_t *)&vlan_subintf_stat_ids->at(0),
                                     stat_val,vlan_subintf_stat_ids->size()) != STD_ERR_OK) {
            EV_LOGGING(INTERFACE,ERR,"NAS-STAT","Failed to get bridge stat for %s",vlan_obj->if_name.c_str());
            return cps_api_ret_code_ERR;
        }
    } else if ((intf_ctrl.int_type == nas_int_type_LAG)){
        if(ndi_lag_bridge_port_stats_get(intf_ctrl.npu_id,intf_ctrl.lag_id,vlan_obj->vlan_id,
                                         (ndi_stat_id_t *)&vlan_subintf_stat_ids->at(0),
                                         stat_val,vlan_subintf_stat_ids->size()) != STD_ERR_OK) {
            EV_LOGGING(INTERFACE,ERR,"NAS-STAT","Failed to get bridge stat for %s",vlan_obj->if_name.c_str());
            return cps_api_ret_code_ERR;
        }
    }

    cps_api_object_t _r_obj = cps_api_object_list_create_obj_and_append(param->list);
    if(_r_obj == nullptr){
        EV_LOGGING(INTERFACE,ERR,"NAS-STAT","Failed to allocate object memory");
        return cps_api_ret_code_ERR;
    }

    for(unsigned int ix = 0 ; ix < vlan_subintf_stat_ids->size() ; ++ix ){
        cps_api_object_attr_add_u64(_r_obj, vlan_subintf_stat_ids->at(ix), stat_val[ix]);
    }

    cps_api_object_attr_add_u32(_r_obj,DELL_BASE_IF_CMN_IF_INTERFACES_STATE_INTERFACE_STATISTICS_TIME_STAMP,time(NULL));

    return cps_api_ret_code_OK;
}


cps_api_return_code_t nas_vlan_sub_intf_stat_clear(cps_api_object_t obj){

    NAS_VLAN_INTERFACE * vlan_obj=nullptr;
    interface_ctrl_t intf_ctrl;

    if(!_fill_vlan_sub_intf_info(obj,vlan_obj,intf_ctrl)){
        return cps_api_ret_code_ERR;
    }

    if ((intf_ctrl.int_type == nas_int_type_PORT)){
        if(ndi_bridge_port_stats_clear(intf_ctrl.npu_id,intf_ctrl.port_id,vlan_obj->vlan_id,
                                       (ndi_stat_id_t *)&vlan_subintf_stat_ids->at(0),
                                       vlan_subintf_stat_ids->size()) != STD_ERR_OK) {
            EV_LOGGING(INTERFACE,ERR,"NAS-STAT","Failed to clear stat for %s",vlan_obj->if_name.c_str());
                return cps_api_ret_code_ERR;
        }
    }
    else if ((intf_ctrl.int_type == nas_int_type_LAG)){
        if(ndi_lag_bridge_port_stats_clear(intf_ctrl.npu_id,intf_ctrl.lag_id,vlan_obj->vlan_id,
                                           (ndi_stat_id_t *)&vlan_subintf_stat_ids->at(0),
                                           vlan_subintf_stat_ids->size()) != STD_ERR_OK) {
            EV_LOGGING(INTERFACE,ERR,"NAS-STAT","Failed to clear stat for %s",vlan_obj->if_name.c_str());
            return cps_api_ret_code_ERR;
        }
    }

    return cps_api_ret_code_OK;
}


static cps_api_return_code_t if_stats_set (void * context, cps_api_transaction_params_t * param,
                                           size_t ix) {

   return cps_api_ret_code_ERR;
}


t_std_error nas_stats_vlan_sub_intf_init(cps_api_operation_handle_t handle) {

    if (intf_obj_handler_registration(obj_INTF_STATISTICS, nas_int_type_VLANSUB_INTF,
                                    if_stats_get, if_stats_set) != STD_ERR_OK) {
        EV_LOG(ERR,INTERFACE, 0,"NAS-STAT", "Failed to register vlab sub interface stats CPS handler");
        return STD_ERR(INTERFACE,FAIL,0);
    }

    return STD_ERR_OK;
}
