/*
 * Copyright (c) 2016 Dell Inc.
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
 * filename: nas_int_ev_handlers.cpp
 * Provides Event handlers for PHY, VLAN and LAG interfaces for the events coming
 * from OS.
 */

#include "event_log.h"
#include "hal_interface_common.h"
#include "hal_if_mapping.h"
#include "nas_int_utils.h"
#include "nas_int_vlan.h"
#include "nas_os_interface.h"
#include "nas_int_bridge.h"
#include "nas_int_lag.h"
#include "nas_int_lag_api.h"
#include "nas_int_com_utils.h"
#include "bridge/nas_interface_bridge_utils.h"
#include "bridge/nas_interface_bridge_com.h"
#include "interface/nas_interface_vlan.h"
#include "interface/nas_interface_vxlan.h"
#include "interface/nas_interface_utils.h"
#include "nas_int_logical.h"
#include "std_error_codes.h"
#include "cps_api_object_category.h"
#include "cps_api_interface_types.h"
#include "dell-base-if-linux.h"
#include "dell-base-if-vlan.h"
#include "dell-base-if.h"
#include "dell-interface.h"
#include "ietf-interfaces.h"
#include "dell-base-common.h"
#include "nas_if_utils.h"
#include "dell-base-interface-common.h"
#include "vrf-mgmt.h"

#include "dell-base-l2-mac.h"
#include "cps_api_object_key.h"
#include "cps_api_operation.h"
#include "ds_common_types.h"
#include "cps_class_map.h"
#include "std_mac_utils.h"
#include "std_ip_utils.h"
#include "bridge/nas_interface_bridge.h"
#include "interface/nas_interface_cps.h"

#include <unordered_map>
#include <string.h>



/* TODO can be moved into a utility file.
static t_std_error get_port_list_by_ifindex(nas_port_list_t &port_list, cps_api_object_t obj, cps_api_attr_id_t attr_id)
{
    cps_api_object_it_t it;
    cps_api_object_it_begin(obj, &it);
    for (;cps_api_object_it_valid(&it) ; cps_api_object_it_next(&it) ) {
        cps_api_attr_id_t id = cps_api_object_attr_id(it.attr);
        if (id == attr_id ) {
            hal_ifindex_t ifindex = cps_api_object_attr_data_u32(it.attr);
            if(!nas_is_virtual_port(ifindex)){
                port_list.insert(ifindex);
            }
        }
    }
    return STD_ERR_OK;
}
*/

static bool nas_intf_cntrl_blk_register(cps_api_object_t obj, interface_ctrl_t *details) {

    hal_intf_reg_op_type_t reg_op;

    if (details == NULL) {
        return false;
    }
    cps_api_operation_types_t op = cps_api_object_type_operation(cps_api_object_key(obj));
    cps_api_object_attr_t if_attr = cps_api_object_attr_get(obj,
            DELL_BASE_IF_CMN_IF_INTERFACES_INTERFACE_IF_INDEX);
    cps_api_object_attr_t _addr = cps_api_object_attr_get(obj,DELL_IF_IF_INTERFACES_INTERFACE_PHYS_ADDRESS);

    cps_api_object_attr_t if_name_attr = cps_api_object_attr_get(obj,
            IF_INTERFACES_INTERFACE_NAME);

    if (if_attr == nullptr || if_name_attr ==nullptr) {
        EV_LOGGING(INTERFACE,INFO,"NAS-INT",
              "if index or if name missing in the OS event ");
        return false;
    }

    const char *name =  (const char*)cps_api_object_attr_data_bin(if_name_attr);
    safestrncpy(details->if_name,name,sizeof(details->if_name));
    details->if_index = cps_api_object_attr_data_u32(if_attr);
    if (_addr != nullptr) {
        const char *mac_addr =  (const char*)cps_api_object_attr_data_bin(_addr);
        safestrncpy(details->mac_addr,mac_addr,sizeof(details->mac_addr));
    }

    if (op == cps_api_oper_CREATE) {
        EV_LOGGING(INTERFACE,INFO,"NAS-INT", "interface register event for %s",
            details->if_name);
        reg_op = HAL_INTF_OP_REG;
    } else if (op == cps_api_oper_DELETE) {
        EV_LOGGING(INTERFACE,INFO,"NAS-INT", "interface de-register event for %s",
            details->if_name);
        reg_op = HAL_INTF_OP_DEREG;
    } else {
        return false;
    }
    if (dn_hal_if_register(reg_op,details) != STD_ERR_OK) {
        EV_LOGGING(INTERFACE,INFO,"NAS-INT",
            "interface register Error %s - mapping error or interface already present",name);
        return false;
    }
    return true;
}
/*  Handle VLAN sub interface Create/delete event */
static void nas_vlansub_intf_ev_handler(cps_api_object_t obj) {
    std_mutex_simple_lock_guard lock(get_vlan_mutex());
    cps_api_operation_types_t op = cps_api_object_type_operation(cps_api_object_key(obj));
    cps_api_object_attr_t if_name_attr = cps_api_object_attr_get(obj,
                IF_INTERFACES_INTERFACE_NAME);
    cps_api_object_attr_t if_attr = cps_api_object_attr_get(obj,
               DELL_BASE_IF_CMN_IF_INTERFACES_INTERFACE_IF_INDEX);
    if(!if_name_attr || !if_attr){
        EV_LOGGING(INTERFACE,ERR,"NAS-VLAN-SUB-INTF","No interface name/index is passed "
                "to create/delete vlan sub intf");
        return;
    }

    std::string _if_name = std::string((const char *)cps_api_object_attr_data_bin(if_name_attr));
    hal_ifindex_t ifindex = cps_api_object_attr_data_uint(if_attr);
    /*  create VLAN sub interface object */
    if (op == cps_api_oper_CREATE) {
        EV_LOGGING(INTERFACE,INFO, "NAS-INTF", " VLAN Interface create name %s %d ",
            _if_name.c_str(), ifindex);
        uint32_t vlan_id;
        cps_api_object_attr_t vlan_id_attr = cps_api_object_attr_get(obj, BASE_IF_VLAN_IF_INTERFACES_INTERFACE_ID);
        cps_api_object_attr_t parent_attr = cps_api_object_attr_get(obj, DELL_IF_IF_INTERFACES_INTERFACE_PARENT_INTERFACE);
        if ((vlan_id_attr == nullptr) || (parent_attr == nullptr)) {
            EV_LOGGING(INTERFACE,INFO,"NAS-INT",
              "VLAN interface OS create/delete event handling failed for %s: Parent intf  or vlan Id missing ", _if_name.c_str());
            return;
        }
        vlan_id = cps_api_object_attr_data_u32(vlan_id_attr);
        const char *parent_name =  (const char*)cps_api_object_attr_data_bin(parent_attr);
        if (nas_interface_utils_vlan_create(_if_name,ifindex,
                                            vlan_id, std::string (parent_name)) != STD_ERR_OK) {
            EV_LOGGING(INTERFACE,ERR,"NAS-INT", "VLAN interface OS create event handling failed: obj creation ");
            return;
        }
    } else if (op == cps_api_oper_DELETE) {
        if (nas_interface_utils_vlan_delete(_if_name) != STD_ERR_OK) {
            EV_LOGGING(INTERFACE,ERR,"NAS-INT", "VLAN interface OS delete event handling failed: obj creation ");
            return;

        }
    }
    interface_ctrl_t details;
    memset(&details,0,sizeof(details));
    details.int_type = nas_int_type_VLANSUB_INTF;

    if ((op == cps_api_oper_CREATE) || (op == cps_api_oper_DELETE)) {
        if (nas_intf_cntrl_blk_register(obj, &details) == false) {
            EV_LOGGING(INTERFACE,INFO,"NAS-INT",
              "VLAN sub interface OS create/delete event handling failed ");
            return;
        }
    }
    nas_interface_cps_publish_event(_if_name,nas_int_type_VLANSUB_INTF, op);
}

/*  Add or delete port in a bridge interface */
//static void nas_vlan_ev_handler(cps_api_object_t obj) {
static void nas_l2_port_ev_handler(cps_api_object_t obj) {
    /*  Now we have two set of DB for bridge for the 1st phase
     *  1. for bridge created by CPS request and with VLAN inform
     *  2. bridge created first in the OS and OPX receives netlink event and reaches here
     *
     *  check if the bridge exists in the VLAN DB if yes then ignore the processing since the
     *  Processing is alerady done at the time of CPS request handling in the NAS-interface
     *
     *  If the bridge exist in the new bridge object map then handle  the member addition/removal
     *  */

    /*  check the bridge existence  */
    /*  TODO use bridge name instead of intf index  */
    cps_api_object_attr_t _br_idx_attr = cps_api_object_attr_get(obj, BASE_IF_LINUX_IF_INTERFACES_INTERFACE_IF_MASTER);
    cps_api_operation_types_t op = cps_api_object_type_operation(cps_api_object_key(obj));
    if (_br_idx_attr == nullptr) {
        EV_LOGGING(INTERFACE,ERR, "NAS-BRIDGE", "VLAN event received without bridge and port info");
        return;
    }
//    hal_ifindex_t br_ifindex = cps_api_object_attr_data_u32(_br_idx_attr);
    /*  check if the bridge exists in the bridge object map */
    /*  If yes then call member addition removal function of the bridge object */

    cps_api_object_attr_t if_name_attr = cps_api_object_attr_get(obj, IF_INTERFACES_INTERFACE_NAME);
    const char *br_name =  (const char*)cps_api_object_attr_data_bin(if_name_attr);

    cps_api_object_attr_t tagged_attr = cps_api_object_attr_get(obj, DELL_IF_IF_INTERFACES_INTERFACE_TAGGED_PORTS);
    cps_api_object_attr_t untagged_attr = cps_api_object_attr_get(obj, DELL_IF_IF_INTERFACES_INTERFACE_UNTAGGED_PORTS);
    EV_LOGGING(INTERFACE,NOTICE,"NAS-INT", " Bridge member add/remove event received %s ", br_name);
    const char *mem_name = nullptr;
    if (tagged_attr != nullptr) {
        mem_name = (const char*)cps_api_object_attr_data_bin(tagged_attr);
    } else if (untagged_attr !=nullptr) {
        mem_name = (const char*)cps_api_object_attr_data_bin(untagged_attr);
    } else {
        EV_LOGGING(INTERFACE,ERR, "NAS-BRIDGE", " NAS OS L2 PORT Event: Missing member information for %s ", br_name);
        return;
    }
    std_mutex_simple_lock_guard _lg(nas_bridge_mtx_lock());

    /*  if the vlan bridge has parent bridge (virtual network) attached then ignore the  events */
    std::string parent_bridge;
    if (nas_bridge_utils_parent_bridge_get(br_name, parent_bridge ) != STD_ERR_OK) {
        EV_LOGGING(INTERFACE,ERR, "NAS-BRIDGE", " Failed to get parent bridge for %s", br_name);
        return;
    }
    if (!parent_bridge.empty()) {
        EV_LOGGING(INTERFACE,NOTICE, "NAS-BRIDGE", " Ignore the OS event for vlan %s  with parent bridge %s",
                         br_name, parent_bridge.c_str());
        return;
    }

    nas_int_type_t mem_type;
    if (nas_get_int_name_type(mem_name, &mem_type) != STD_ERR_OK) {
        EV_LOGGING(INTERFACE,ERR, "NAS-BRIDGE", " NAS OS L2 PORT Event: Failed to get member type %s ", mem_name);
        return;
    }
    /*  Handle member addition */
    bool present = false;
    if (op == cps_api_oper_CREATE) {
        EV_LOGGING(INTERFACE,NOTICE,"NAS-INT", " Bridge member add event received %s ", br_name);
        if((nas_bridge_utils_check_membership(br_name, mem_name, &present) == STD_ERR_OK) && (present)) {
            EV_LOGGING(INTERFACE,NOTICE,"NAS-INT", " Bridge member %s already present in %s ", mem_name, br_name);
            return;

        }
        if (nas_bridge_utils_npu_add_member(br_name, mem_type, mem_name) != STD_ERR_OK) {
            EV_LOGGING(INTERFACE,ERR, "NAS-BRIDGE", " NAS OS L2 PORT Event: Failed to add member %s to bridge %s ",
                                     mem_name, br_name);
            return;
        }
    } else if (op == cps_api_oper_DELETE) {
        if((nas_bridge_utils_check_membership(br_name, mem_name, &present) == STD_ERR_OK) && (!present)) {
            EV_LOGGING(INTERFACE,NOTICE,"NAS-INT", " Bridge member %s  not present in %s ", mem_name, br_name);
            return;

        }
    /*  Handle Member removal  */
        EV_LOGGING(INTERFACE,NOTICE,"NAS-INT", " Bridge member remove event received %s ", br_name);
        if (nas_bridge_utils_npu_remove_member(br_name, mem_type, mem_name) != STD_ERR_OK) {
            EV_LOGGING(INTERFACE,ERR, "NAS-BRIDGE", " NAS OS L2 PORT Event: Failed to remove member %s from bridge %s ",
                                     mem_name, br_name);
            return;
        }
    }
    nas_bridge_utils_publish_member_event(std::string(br_name), std::string(mem_name),  op);
}

/*  Handles VXLAN interface create and delete event from nas-linux */
void nas_vxlan_ev_handler(cps_api_object_t obj)
{
    std_mutex_simple_lock_guard lock(get_vxlan_mutex());
    cps_api_operation_types_t op = cps_api_object_type_operation(cps_api_object_key(obj));
    cps_api_object_attr_t name_attr = cps_api_object_attr_get(obj, IF_INTERFACES_INTERFACE_NAME);
    if(!name_attr){
        EV_LOGGING(INTERFACE,ERR,"NAS-VXLAN-EVENET","No name passed for vxlan event");
        return;
    }
    std::string _if_name = std::string((const char *)cps_api_object_attr_data_bin(name_attr));
    BASE_CMN_AF_TYPE_t af_type;
    /*  create/delete vxlan object and handle it */
    if (op == cps_api_oper_CREATE) {
        hal_ip_addr_t local_ip;
        cps_api_object_attr_t vni_attr = cps_api_object_attr_get(obj, DELL_IF_IF_INTERFACES_INTERFACE_VNI);
        cps_api_object_attr_t _ip_addr = cps_api_object_attr_get(obj, DELL_IF_IF_INTERFACES_INTERFACE_SOURCE_IP_ADDR);
        cps_api_object_attr_t _af_type = cps_api_object_attr_get(obj, DELL_IF_IF_INTERFACES_INTERFACE_SOURCE_IP_ADDR_FAMILY);
        cps_api_object_attr_t if_attr = cps_api_object_attr_get(obj,
                    DELL_BASE_IF_CMN_IF_INTERFACES_INTERFACE_IF_INDEX);
        af_type = (BASE_CMN_AF_TYPE_t)cps_api_object_attr_data_u32(_af_type);
        if(af_type == AF_INET) {
             struct in_addr *inp = (struct in_addr *) cps_api_object_attr_data_bin(_ip_addr);
             std_ip_from_inet(&local_ip,inp);
        } else {
            struct in6_addr *inp6 = (struct in6_addr *) cps_api_object_attr_data_bin(_ip_addr);
            std_ip_from_inet6(&local_ip,inp6);
        }
        hal_ifindex_t ifindex = 0;
        if(if_attr){
            ifindex = cps_api_object_attr_data_uint(if_attr);
        }
        BASE_CMN_VNI_t vni = (BASE_CMN_VNI_t ) cps_api_object_attr_data_u32(vni_attr);
        if (nas_interface_utils_vxlan_create(_if_name, ifindex, vni, local_ip)
                != STD_ERR_OK) {
            EV_LOGGING(INTERFACE,INFO,"NAS-INT",
              "VXLAN interface OS create event handling failed %s", _if_name.c_str());
            return;
        }
    } else if (op == cps_api_oper_DELETE) {

        NAS_VXLAN_INTERFACE * vxlan_obj = dynamic_cast<NAS_VXLAN_INTERFACE *>(nas_interface_map_obj_get(_if_name));

        if(vxlan_obj == nullptr){
            EV_LOGGING(INTERFACE,ERR,"VXLAN","No entry for vxlan interface %s exist",_if_name.c_str());
            return;
        }

        if(!vxlan_obj->create_in_os()){
            if (nas_interface_utils_vxlan_delete(_if_name) != STD_ERR_OK) {
                EV_LOGGING(INTERFACE,INFO,"NAS-INT",
                        "VXLAN interface OS delete event handling failed %s", _if_name.c_str());
                return;
            }
        }else{
            EV_LOGGING(INTERFACE,INFO,"NAS-VXLAN-EVENT","Dont update vxlan object created via cps");
            return;
        }
    } else {
        /*  TODO Add support for set operation */
        EV_LOGGING(INTERFACE,INFO,"NAS-INT", "Wrong operation for vxlan intf %s ",_if_name.c_str());
        return;
    }
    interface_ctrl_t details;
    memset(&details,0,sizeof(details));
    details.int_type = nas_int_type_VXLAN;
    if ((op == cps_api_oper_CREATE) || (op == cps_api_oper_DELETE)) {
        if (nas_intf_cntrl_blk_register(obj, &details) == false) {
            EV_LOGGING(INTERFACE,INFO,"NAS-INT",
             "VXLAN interface OS create/delete event handling failed: ");
            return;
        }
    }
    nas_interface_cps_publish_event(_if_name,nas_int_type_VXLAN, op);
}
/*  Handles LAG interface create and delete event from nas-linux */
static void nas_lag_ev_set_handler(hal_ifindex_t bond_idx, cps_api_object_t obj, bool create) {

    /* config Admin and MAC attribute */
    cps_api_object_attr_t _mac = cps_api_object_attr_get(obj,
                                                DELL_IF_IF_INTERFACES_INTERFACE_PHYS_ADDRESS);
    if (_mac != nullptr && create == false) {
        nas_lag_set_mac(bond_idx, (const char *)cps_api_object_attr_data_bin(_mac));
    }
    cps_api_object_attr_t _enabled = cps_api_object_attr_get(obj, IF_INTERFACES_INTERFACE_ENABLED);
    if (_enabled != nullptr) {
        nas_lag_set_admin_status(bond_idx, (bool)cps_api_object_attr_data_u32(_enabled));
    }
}

void nas_lag_ev_handler(cps_api_object_t obj) {

    nas_lag_id_t lag_id = 0; // @TODO for now lag_id=0
    hal_ifindex_t mem_idx;
    bool create = false;
    bool block_status = true;
    const char *mem_name = NULL;
    nas_lag_master_info_t *nas_lag_entry=NULL;
    cps_api_operation_types_t member_op = cps_api_oper_NULL;

    cps_api_object_attr_t _idx_attr = cps_api_object_attr_get(obj,
                                        DELL_BASE_IF_CMN_IF_INTERFACES_INTERFACE_IF_INDEX);
    cps_api_object_attr_t _mem_attr = cps_api_object_attr_get(obj,
                                     DELL_IF_IF_INTERFACES_INTERFACE_MEMBER_PORTS_NAME);
    cps_api_object_attr_t _name_attr = cps_api_object_attr_get(obj, IF_INTERFACES_INTERFACE_NAME);
    cps_api_operation_types_t op = cps_api_object_type_operation(cps_api_object_key(obj));

    if (_idx_attr == nullptr) {
        EV_LOGGING(INTERFACE,ERR, "NAS-LAG", " LAG IF_index not present in the OS EVENT");
        return;
    }
    hal_ifindex_t bond_idx = cps_api_object_attr_data_u32(_idx_attr);
    if (_mem_attr != nullptr) {
        mem_name =  (const char*)cps_api_object_attr_data_bin(_mem_attr);
        if (nas_int_name_to_if_index(&mem_idx, mem_name) != STD_ERR_OK) {
            EV_LOGGING(INTERFACE,ERR, "NAS-LAG",
                    " Failed for get member %s if_index ", mem_name);
            return;
        }

        if_master_info_t master_info = { nas_int_type_LAG, NAS_PORT_NONE, bond_idx};
        BASE_IF_MODE_t intf_mode = nas_intf_get_mode(mem_idx);
        if (op == cps_api_oper_CREATE) {
            if(!nas_intf_add_master(mem_idx, master_info)){
                EV_LOGGING(INTERFACE,DEBUG,"NAS-LAG","Failed to add master for lag memeber port");
            } else {
                BASE_IF_MODE_t new_mode = nas_intf_get_mode(mem_idx);
                if (new_mode != intf_mode) {
                    if (nas_intf_handle_intf_mode_change(mem_idx, new_mode) == false) {
                        EV_LOGGING(INTERFACE,DEBUG,"NAS-LAG",
                                "Update to NAS-L3 about interface mode change failed(%d)", mem_idx);
                    }
                }
            }
        } else if (op == cps_api_oper_DELETE) {
             nas_lag_master_info_t * lag_entry =NULL;
             /*  delete the member from the lag */
             lag_entry = nas_get_lag_node(bond_idx);
             if(lag_entry == nullptr){
                 EV_LOGGING(INTERFACE,INFO,"NAS-LAG","Failed to find lag entry with %d"
                              "ifindex for delete operation",bond_idx);
                 return;
             }

            /*
             * For kernel notification to delete the member port, check if present in block list
             * if it is in blocking list then we would have removed the port from kernel to prevent
             * hashing to blocked port in kernel. In that case just continue and don't trigger
             * mode change
             */

            if(lag_entry->block_port_list.find(mem_idx) != lag_entry->block_port_list.end()){
                return;
            }

            if(!nas_intf_del_master(mem_idx, master_info)){
                 EV_LOGGING(INTERFACE,DEBUG,"NAS-LAG",
                         "Failed to delete master for lag memeber port");
            } else {
                BASE_IF_MODE_t new_mode = nas_intf_get_mode(mem_idx);
                if (new_mode != intf_mode) {
                    if (nas_intf_handle_intf_mode_change(mem_idx, new_mode) == false) {
                        EV_LOGGING(INTERFACE,DEBUG,"NAS-LAG",
                                "Update to NAS-L3 about interface mode change failed(%d)", mem_idx);
                    }
                }
            }
        }

        if(nas_is_virtual_port(mem_idx)){
             EV_LOGGING(INTERFACE,INFO, "NAS-LAG",
                     " Member port %s is virtual no need to do anything", mem_name);
             return;
        }
            /* TODO Add function to set intf description on netlink message */
    }

    std_mutex_simple_lock_guard lock_t(nas_lag_mutex_lock());
    /* If op is CREATE then create the lag if not present and then add member lists*/
    if (op == cps_api_oper_CREATE) {
        if (_name_attr == nullptr) {
            EV_LOGGING(INTERFACE,ERR, "NAS-LAG", " Bond name is not present in the create event");
            return;
        }
        const char *bond_name =  (const char*)cps_api_object_attr_data_bin(_name_attr);
        if ((nas_lag_entry = nas_get_lag_node(bond_idx)) == NULL) {
            /*  Create the lag  */
            EV_LOGGING(INTERFACE,INFO,"NAS-LAG","Create Lag interface idx %d with lag ID %d ", bond_idx,lag_id);

            if ((nas_lag_master_add(bond_idx,bond_name,lag_id)) != STD_ERR_OK) {
                    EV_LOGGING(INTERFACE,ERR, "NAS-LAG",
                            "LAG creation Failed for bond interface %s index %d",bond_name, bond_idx);
                    return;
            }
            if (nas_intf_handle_intf_mode_change(bond_idx, BASE_IF_MODE_MODE_NONE) == false) {
                EV_LOGGING(INTERFACE, DEBUG, "NAS-LAG",
                        "Update to NAS-L3 about interface mode change failed(%d)", bond_idx);
            }
            /* Now get the lag node entry */
            nas_lag_entry = nas_get_lag_node(bond_idx);
            if(nas_lag_entry != NULL){
                nas_cps_handle_mac_set (bond_name, nas_lag_entry->ifindex);
                create = true;
            }
            /* TODO Add function to set intf description on netlink message */
        }
        /*   Check if Member port is present then add the members in the lag */
        if (_mem_attr != nullptr) {
              /* Check: if port is a bond member*/
            if (nas_lag_if_port_is_lag_member(bond_idx, mem_idx)) {
                EV_LOGGING(INTERFACE, DEBUG, "NAS-LAG", "Slave port %d already a member of lag %d",
                               mem_idx, bond_idx);
                return ;
            }
            if(nas_lag_member_add(bond_idx,mem_idx) != STD_ERR_OK) {
                EV_LOGGING(INTERFACE,INFO, "NAS-LAG",
                    "Failed to Add member %s to the Lag %s", mem_name, bond_name);
                return ;
            }
            nas_lag_entry->port_list.insert(mem_idx);
            member_op = cps_api_oper_SET;
            ndi_port_t ndi_port;
            if (nas_int_get_npu_port(mem_idx, &ndi_port) != STD_ERR_OK) {
                EV_LOGGING(INTERFACE,ERR, "NAS-LAG",
                        "Error in finding member's %s npu port info", mem_name);
                return;
            }
            ndi_intf_link_state_t state;
            if ((ndi_port_link_state_get(ndi_port.npu_id, ndi_port.npu_port, &state))
                     == STD_ERR_OK) {

                /* if Oper up and port not in block list, then reset egress_disable */
                if ((nas_lag_entry->block_port_list.find(mem_idx) ==
                     nas_lag_entry->block_port_list.end()) &&
                     (state.oper_status == ndi_port_OPER_UP)) {

                    block_status = false;
                }

                if (nas_lag_block_port(nas_lag_entry,mem_idx,block_status) != STD_ERR_OK){

                    EV_LOGGING(INTERFACE,ERR, "NAS-CPS-LAG",
                            "Error Block/unblock Port %s lag %s ",mem_name, bond_name);
                    return ;
                }
            }
        }
        // Handler attribute admin,MAC update
        nas_lag_ev_set_handler(bond_idx, obj, create);

    } else if (op == cps_api_oper_DELETE) { /* If op is DELETE */
        if (_mem_attr != nullptr) {
            /*  delete the member from the lag */
            nas_lag_entry = nas_get_lag_node(bond_idx);
            if(nas_lag_entry == nullptr){
                EV_LOGGING(INTERFACE,INFO,"NAS-LAG","Failed to find lag entry with %d"
                        "ifindex for delete operation",bond_idx);
                return;
            }

            /*
             * For kernel notification to delete the member port, check if present in block list
             * if it is in blocking list then we would have removed the port from kernel to prevent
             * hashing to blocked port in kernel. In that case just continue and let the port be
             * still there in npu as part of lag
             */
            if(nas_lag_entry->block_port_list.find(mem_idx) != nas_lag_entry->block_port_list.end()){
                return;
            }

            if(nas_lag_member_delete(bond_idx, mem_idx) != STD_ERR_OK) {
                EV_LOGGING(INTERFACE,INFO,"NAS-LAG",
                        "Failed to Delete member %s to the Lag %d", mem_name, bond_idx);
                return ;
            }
            nas_lag_entry->port_list.erase(mem_idx);
            member_op = cps_api_oper_SET;
        } else {
            if (nas_intf_handle_intf_mode_change(bond_idx, BASE_IF_MODE_MODE_L2) == false) {
                EV_LOGGING(INTERFACE, DEBUG, "NAS-LAG",
                        "Update to NAS-L3 about interface mode change failed(%d)", bond_idx);
            }
            /*  Otherwise the event is to delete the lag */
            if((nas_lag_master_delete(bond_idx) != STD_ERR_OK))
                return ;
        }
    } else if (op == cps_api_oper_SET) {
        EV_LOGGING(INTERFACE, DEBUG, "NAS-LAG", "LAG set event received for %d", bond_idx);
        nas_lag_ev_set_handler(bond_idx, obj, create);
    }
    if (member_op == cps_api_oper_SET) {
        /*  Publish the Lag event with portlist in case of member addition/deletion */
        if ((nas_lag_entry = nas_get_lag_node(bond_idx)) == NULL) {
            return;
        }
        if(lag_object_publish(nas_lag_entry, bond_idx, member_op)!= cps_api_ret_code_OK){
            EV_LOGGING(INTERFACE,ERR, "NAS-CPS-LAG",
                    "LAG events publish failure");
            return ;
        }
    }
}

// Bridge Event Handler
static void nas_bridge_ev_handler(cps_api_object_t obj)
{
    cps_api_operation_types_t op = cps_api_object_type_operation(cps_api_object_key(obj));
    cps_api_object_attr_t if_name_attr = cps_api_object_attr_get(obj,
            IF_INTERFACES_INTERFACE_NAME);
    cps_api_object_attr_t if_attr = cps_api_object_attr_get(obj,
            DELL_BASE_IF_CMN_IF_INTERFACES_INTERFACE_IF_INDEX);
    if ((if_name_attr ==nullptr) || (if_attr == nullptr)) {
        EV_LOGGING(INTERFACE,ERR,"NAS-INT", "if name missing in the OS event ");
        return;
    }
    const char *br_name =  (const char*)cps_api_object_attr_data_bin(if_name_attr);

    std_mutex_simple_lock_guard _lg(nas_bridge_mtx_lock());

    hal_ifindex_t idx = cps_api_object_attr_data_u32(if_attr);
    EV_LOGGING(INTERFACE,NOTICE,"NAS-INT", " Bridge create /delete event received %s ", br_name);
    // TODO Handle Set MAc Learning operation.
    /*  TODO Add bridge lock  */
    if ( op == cps_api_oper_CREATE) {

        if( nas_bridge_utils_if_bridge_exists(br_name) == STD_ERR_OK) {
            EV_LOGGING(INTERFACE,ERR,"NAS-INT", " Bridge obj already exists for bridge %s", br_name);
            return;
        }
        /*  Else create newer bridge object
         *  and save in the bridge name to bridge object map
         *  */
        EV_LOGGING(INTERFACE,NOTICE,"NAS-INT", " Bridge obj create ");
        if (nas_bridge_utils_create_obj(br_name, BASE_IF_BRIDGE_MODE_1Q, idx) != STD_ERR_OK) {
            EV_LOGGING(INTERFACE,ERR,"NAS-INT", " Bridge obj create failed ");
            return;
        }
        if (nas_intf_handle_intf_mode_change(br_name, BASE_IF_MODE_MODE_NONE) == false) {
            EV_LOGGING(INTERFACE, DEBUG, "NAS-BR",
                    "Update to NAS-L3 about interface mode change failed(%s)", br_name);
        }
    } else if (op == cps_api_oper_DELETE) {
        // TODO Assumption is that member list is already clean-up before processing the
        // bridge delete event from NAS-OS.
     /*     simply check in the new bridge object. */
        if( nas_bridge_utils_if_bridge_exists(br_name) != STD_ERR_OK) {
            EV_LOGGING(INTERFACE,ERR,"NAS-INT-EVT", " Bridge obj does not exists for bridge %s", br_name);
            return;
        }
        EV_LOGGING(INTERFACE,NOTICE,"NAS-INT", " Bridge delete event received ");
        if (nas_intf_handle_intf_mode_change(br_name, BASE_IF_MODE_MODE_L2) == false) {
            EV_LOGGING(INTERFACE, DEBUG, "NAS-BR",
                    "Update to NAS-L3 about interface mode change failed(%s)", br_name);
        }
        EV_LOGGING(INTERFACE,NOTICE,"NAS-INT", " Bridge delete ");
        if (nas_bridge_utils_delete(br_name) != STD_ERR_OK) {
            EV_LOGGING(INTERFACE,ERR,"NAS-INT", " Bridge obj already deleted or does not exist %s", br_name);
            return;
        }
    }
    EV_LOGGING(INTERFACE,NOTICE,"NAS-INT", " Bridge create/delete event publish ");
    nas_bridge_utils_publish_event(br_name, op);
    return;
}

void nas_send_admin_state_event(hal_ifindex_t if_index, IF_INTERFACES_STATE_INTERFACE_ADMIN_STATUS_t state)
{

    char buff[CPS_API_MIN_OBJ_LEN];
    char if_name[HAL_IF_NAME_SZ];    //! the name of the interface
    cps_api_object_t obj = cps_api_object_init(buff,sizeof(buff));
    if (!cps_api_key_from_attr_with_qual(cps_api_object_key(obj),
                DELL_BASE_IF_CMN_IF_INTERFACES_STATE_INTERFACE_OBJ,
                cps_api_qualifier_OBSERVED)) {
        EV_LOGGING(INTERFACE,ERR,"NAS-IF-REG","Could not translate to logical interface key ");
        return;
    }
    if (nas_int_get_if_index_to_name(if_index, if_name, sizeof(if_name)) != STD_ERR_OK) {
        EV_LOGGING(INTERFACE,ERR,"NAS-INT","Could not find interface name for if_index %d", if_index);
        return;
    }
    cps_api_object_attr_add(obj, IF_INTERFACES_STATE_INTERFACE_NAME, if_name, strlen(if_name) + 1);
    cps_api_object_attr_add_u32(obj, IF_INTERFACES_STATE_INTERFACE_IF_INDEX, if_index);
    cps_api_object_attr_add_u32((obj), IF_INTERFACES_STATE_INTERFACE_ADMIN_STATUS, state);
    hal_interface_send_event(obj);
}

static void send_lpbk_intf_oper_event(cps_api_object_t obj)
{
    cps_api_object_attr_t _enabled_attr = cps_api_object_attr_get(obj, IF_INTERFACES_INTERFACE_ENABLED);
    if (_enabled_attr == nullptr) return;

    if (!cps_api_key_from_attr_with_qual(cps_api_object_key(obj),
                DELL_BASE_IF_CMN_IF_INTERFACES_STATE_INTERFACE_OBJ,
                cps_api_qualifier_OBSERVED)) {
        EV_LOGGING(INTERFACE,ERR,"NAS-IF","Could not translate to logical interface key ");
        return;
    }
    bool _enabled = (bool) cps_api_object_attr_data_u32(_enabled_attr);
    IF_INTERFACES_STATE_INTERFACE_OPER_STATUS_t _oper_state =  (_enabled) ?
        IF_INTERFACES_STATE_INTERFACE_OPER_STATUS_UP : IF_INTERFACES_STATE_INTERFACE_OPER_STATUS_DOWN;

    cps_api_object_attr_add_u32(obj,IF_INTERFACES_STATE_INTERFACE_OPER_STATUS,_oper_state);
    hal_interface_send_event(obj);

}

static void nas_loopback_ev_handler(cps_api_object_t obj)
{
    interface_ctrl_t details;
    hal_intf_reg_op_type_t reg_op;

    memset(&details,0,sizeof(details));
    cps_api_operation_types_t op = cps_api_object_type_operation(cps_api_object_key(obj));
    cps_api_object_attr_t if_attr = cps_api_object_attr_get(obj,
            DELL_BASE_IF_CMN_IF_INTERFACES_INTERFACE_IF_INDEX);
    cps_api_object_attr_t if_name_attr = cps_api_object_attr_get(obj,
            IF_INTERFACES_INTERFACE_NAME);
    if (if_attr == nullptr || if_name_attr == nullptr) {
        EV_LOGGING(INTERFACE,INFO,"NAS-INT",
                   "if index or if name missing for loopback interface type ");
        return ; // if index or name not present
    }

    const char *name =  (const char*)cps_api_object_attr_data_bin(if_name_attr);
    safestrncpy(details.if_name,name,sizeof(details.if_name));
    details.if_index = cps_api_object_attr_data_u32(if_attr);
    details.int_type = nas_int_type_LPBK;
    details.desc = nullptr;

    if_name_attr = cps_api_object_attr_get(obj, IF_INTERFACES_STATE_INTERFACE_NAME);
    if (if_name_attr == nullptr) {
        cps_api_object_attr_delete(obj, IF_INTERFACES_INTERFACE_NAME);
        name = details.if_name;
        cps_api_object_attr_add(obj, IF_INTERFACES_STATE_INTERFACE_NAME,
                                name, strlen(name) + 1);
    }

    reg_op = HAL_INTF_OP_REG;
    if (op == cps_api_oper_CREATE) {
        EV_LOGGING(INTERFACE,INFO,"NAS-INT", "interface register event for %s",name);
    } else if (op == cps_api_oper_DELETE) {
        EV_LOGGING(INTERFACE,INFO,"NAS-INT", "interface de-register event for %s",name);
        reg_op = HAL_INTF_OP_DEREG;
    } else {
        EV_LOGGING(INTERFACE,INFO,"NAS-INT", "interface Loopback set event for %s", name);
    }
    if (op != cps_api_oper_SET) {
        if (dn_hal_if_register(reg_op,&details)!=STD_ERR_OK) {
            EV_LOGGING(INTERFACE,INFO,"NAS-INT",
                "interface register Error %s - mapping error or loopback interface already present",name);
        }
    }
    send_lpbk_intf_oper_event(obj);
    return;
}

/*  Update the MACVLAN intf create/deletion in the intf control block */
static void nas_macvlan_ev_handler(cps_api_object_t obj)
{
    interface_ctrl_t details;

    memset(&details,0,sizeof(details));
    cps_api_operation_types_t op = cps_api_object_type_operation(cps_api_object_key(obj));
    cps_api_object_attr_t if_attr = cps_api_object_attr_get(obj,
            DELL_BASE_IF_CMN_IF_INTERFACES_INTERFACE_IF_INDEX);

    cps_api_object_attr_t if_name_attr = cps_api_object_attr_get(obj,
            IF_INTERFACES_INTERFACE_NAME);

    if (if_attr == nullptr || if_name_attr ==nullptr) {
        EV_LOGGING(INTERFACE,INFO,"NAS-INT",
              "if index or if name missing for MAC VLAN interface type ");
        return ;
    }

    const char *name =  (const char*)cps_api_object_attr_data_bin(if_name_attr);
    if (op == cps_api_oper_DELETE) {
        interface_ctrl_t info;
        memset(&info, 0, sizeof(info));
        safestrncpy(info.if_name, name, strlen(info.if_name)+1);
        info.q_type = HAL_INTF_INFO_FROM_IF_NAME;
        if (dn_hal_get_interface_info(&info) == STD_ERR_OK) {
            if (info.vrf_id != NAS_DEFAULT_VRF_ID) {
                /* Since we're moving the MAC-VLAN interface from default to non-default VRF,
                 * it is possible that MAC-VLAN delete from default VRF could delete
                 * the already exising MAC_VLAN interface with different vrf-id,
                 * if there is a VRF-id difference, ignore the update.
                 * @@TODO Enhance NAS-Common intf-name to intf-info map
                 * to include vrf-id also as the key */
                EV_LOGGING(INTERFACE,INFO,"NAS-INT", "Intf:%s delete ignored since "
                           "the interface is associated with VRF:%d", name, info.vrf_id);
                return;
            }
        }
    }

    safestrncpy(details.if_name,name,sizeof(details.if_name));
    details.if_index = cps_api_object_attr_data_u32(if_attr);
    details.int_type = nas_int_type_MACVLAN;

    if ((op == cps_api_oper_CREATE) || (op == cps_api_oper_DELETE)) {
        if (nas_intf_cntrl_blk_register(obj, &details) == false) {
            EV_LOGGING(INTERFACE,INFO,"NAS-INT",
              "MCVLAN interface OS create/delete event handling failed ");
            return;
        }
    }
}

void nas_int_ev_handler(cps_api_object_t obj) {

    hal_ifindex_t if_index;
    ndi_port_t ndi_port;

    cps_api_object_attr_t attr = cps_api_object_attr_get(obj,DELL_BASE_IF_CMN_IF_INTERFACES_INTERFACE_IF_INDEX);
    if (attr==nullptr) return ; // if index not present
    if_index = cps_api_object_attr_data_u32(attr);
    if (nas_is_virtual_port(if_index)) {
        EV_LOGGING(INTERFACE, DEBUG, "NAS-INT", "Bypass virtual interface, ifindex=%d", if_index);
        return;
    }
    if (nas_int_get_npu_port(if_index, &ndi_port) != STD_ERR_OK) {
        EV_LOGGING(INTERFACE,DEBUG,"NAS-INT","if_index %d is wrong or Interface has NO slot port ", if_index);
        return;
    }

    std_mutex_simple_lock_guard g(nas_physical_intf_lock());
    /*
     * Admin State
     */
    attr = cps_api_object_attr_get(obj,IF_INTERFACES_INTERFACE_ENABLED);
    if (attr!=nullptr) {
        IF_INTERFACES_STATE_INTERFACE_ADMIN_STATUS_t state, current_state;
        state = (bool)cps_api_object_attr_data_u32(attr) ?
                IF_INTERFACES_STATE_INTERFACE_ADMIN_STATUS_UP : IF_INTERFACES_STATE_INTERFACE_ADMIN_STATUS_DOWN;
        if (ndi_port_admin_state_get(ndi_port.npu_id, ndi_port.npu_port,&current_state)==STD_ERR_OK) {
            if (current_state != state) {
                if (ndi_port_admin_state_set(ndi_port.npu_id, ndi_port.npu_port,
                            (state == IF_INTERFACES_STATE_INTERFACE_ADMIN_STATUS_UP) ? true: false) != STD_ERR_OK) {
                    EV_LOGGING(INTERFACE,ERR,"INTF-NPU","Error Setting Admin State to %s for %d:%d ifindex %d",
                               state == IF_INTERFACES_STATE_INTERFACE_ADMIN_STATUS_UP ? "UP" : "DOWN",
                               ndi_port.npu_id, ndi_port.npu_port, if_index);
                } else {
                    EV_LOGGING(INTERFACE,INFO,"INTF-NPU","Admin state change on %d:%d to %d",
                        ndi_port.npu_id, ndi_port.npu_port,state);
                }
                /*  Update in local cache and publish the msg */
                nas_intf_admin_state_set(if_index, state);
                nas_send_admin_state_event(if_index, state);
            }
        } else {
            /*  port admin state get error */
                EV_LOGGING(INTERFACE,ERR,"INTF-NPU","Admin state Get failed on %d:%d ",
                    ndi_port.npu_id, ndi_port.npu_port);
        }
    }
    /*
     * handle ip mtu
     */
    attr = cps_api_object_attr_get(obj,DELL_IF_IF_INTERFACES_INTERFACE_MTU);
    if (attr!=NULL) {
        uint_t mtu = cps_api_object_attr_data_u32(attr);
        auto npu = ndi_port.npu_id;
        auto port = ndi_port.npu_port;

        if (ndi_port_mtu_set(npu,port, mtu)!=STD_ERR_OK) {
            /* If unable to set new port MTU (based on received MTU) in NPU
               then revert back the MTU in kernel and MTU in NPU to old values */
            EV_LOGGING(INTERFACE,ERR,"INTF-NPU","Error setting MTU %d for NPU %d PORT %d ifindex %d",
                       mtu, npu, port, if_index);
            if (ndi_port_mtu_get(npu,port,&mtu)==STD_ERR_OK) {
                cps_api_set_key_data(obj, DELL_BASE_IF_CMN_IF_INTERFACES_INTERFACE_IF_INDEX, cps_api_object_ATTR_T_U32,
                             &if_index, sizeof(if_index));
                cps_api_object_attr_delete(obj,DELL_IF_IF_INTERFACES_INTERFACE_MTU);
                cps_api_object_attr_add_u32(obj,DELL_IF_IF_INTERFACES_INTERFACE_MTU,mtu);
                nas_os_interface_set_attribute(obj,DELL_IF_IF_INTERFACES_INTERFACE_MTU);
            }
        }
    }
}

static auto _int_ev_handlers = new std::unordered_map<BASE_CMN_INTERFACE_TYPE_t, void(*)
                                                (cps_api_object_t), std::hash<int>>
{
    { BASE_CMN_INTERFACE_TYPE_L3_PORT, nas_int_ev_handler},
    { BASE_CMN_INTERFACE_TYPE_BRIDGE, nas_bridge_ev_handler},
    { BASE_CMN_INTERFACE_TYPE_VLAN_SUBINTF, nas_vlansub_intf_ev_handler},
    { BASE_CMN_INTERFACE_TYPE_VXLAN, nas_vxlan_ev_handler},
    { BASE_CMN_INTERFACE_TYPE_L2_PORT, nas_l2_port_ev_handler},
    { BASE_CMN_INTERFACE_TYPE_LAG, nas_lag_ev_handler},
    { BASE_CMN_INTERFACE_TYPE_MACVLAN, nas_macvlan_ev_handler},
    { BASE_CMN_INTERFACE_TYPE_LOOPBACK, nas_loopback_ev_handler},
};

bool nas_int_ev_handler_cb(cps_api_object_t obj, void *param) {

    cps_api_object_attr_t _type = cps_api_object_attr_get(obj,BASE_IF_LINUX_IF_INTERFACES_INTERFACE_DELL_TYPE);
    if (_type==nullptr) {
        EV_LOGGING(INTERFACE,ERR,"INTF-EV","Unknown Event or interface type not present");
        return false;
    }
    EV_LOGGING(INTERFACE,INFO,"INTF-EV","OS event received for interface state change.");
    BASE_CMN_INTERFACE_TYPE_t if_type = (BASE_CMN_INTERFACE_TYPE_t) cps_api_object_attr_data_u32(_type);
    cps_api_object_attr_t _vrf_attr = cps_api_object_attr_get(obj, VRF_MGMT_NI_IF_INTERFACES_INTERFACE_VRF_ID);
    if (_vrf_attr && (cps_api_object_attr_data_u32(_vrf_attr) != NAS_DEFAULT_VRF_ID)) {
        /* Ignore all interface events interface from non-default VRF */
        return true;
    }
    auto func = _int_ev_handlers->find(if_type);
    if (func != _int_ev_handlers->end()) {
        func->second(obj);
        return true;
    } else {
        EV_LOGGING(INTERFACE,ERR,"INTF-EV","Unknown interface type");
        return false;
    }
}


static bool nas_vxlan_remote_endpoint_handler_cb(cps_api_object_t obj, void *param)
{
    cps_api_operation_types_t op = cps_api_object_type_operation(cps_api_object_key(obj));
    cps_api_object_attr_t _ip_addr = cps_api_object_attr_get(obj, BASE_MAC_TUNNEL_ENDPOINT_EVENT_IP_ADDR);
    cps_api_object_attr_t _af_type = cps_api_object_attr_get(obj, BASE_MAC_TUNNEL_ENDPOINT_EVENT_IP_ADDR_FAMILY);
    cps_api_object_attr_t _vxlan_if = cps_api_object_attr_get(obj, BASE_MAC_TUNNEL_ENDPOINT_EVENT_INTERFACE_NAME);
    cps_api_object_attr_t _flooding_enable = cps_api_object_attr_get(obj, BASE_MAC_TUNNEL_ENDPOINT_EVENT_FLOODING_ENABLE);

    if ((_ip_addr == nullptr) || (_af_type == nullptr) || (_vxlan_if == nullptr) || (_flooding_enable ==nullptr)) {
        return true;
    }

    std_mutex_simple_lock_guard _lg(nas_bridge_mtx_lock());

    const char *vxlan_if =  (const char*)cps_api_object_attr_data_bin(_vxlan_if);

    remote_endpoint_t rem_ep;
    rem_ep.flooding_enabled = (bool) cps_api_object_attr_data_u32(_flooding_enable);
    rem_ep.remote_ip.af_index = (BASE_CMN_AF_TYPE_t)cps_api_object_attr_data_u32(_af_type);
    if(rem_ep.remote_ip.af_index == AF_INET) {
         memcpy(&rem_ep.remote_ip.u.ipv4,cps_api_object_attr_data_bin(_ip_addr),
                 sizeof(rem_ep.remote_ip.u.ipv4));
    } else {
         memcpy(&rem_ep.remote_ip.u.ipv6,cps_api_object_attr_data_bin(_ip_addr),
                         sizeof(rem_ep.remote_ip.u.ipv6));
    }

    if (op == cps_api_oper_CREATE) {
        if(nas_bridge_utils_add_remote_endpoint(vxlan_if, rem_ep) != STD_ERR_OK) {
            EV_LOGGING(INTERFACE,ERR,"INTF-EV","Failed to add remote endpoint");
        }
    } else if (op == cps_api_oper_DELETE) {
        if(nas_bridge_utils_remove_remote_endpoint(vxlan_if, rem_ep) != STD_ERR_OK) {
            EV_LOGGING(INTERFACE,ERR,"INTF-EV","Failed to add remote endpoint");
        }
    }else if(op == cps_api_oper_SET){
        if(nas_bridge_utils_update_remote_endpoint(vxlan_if, rem_ep)!=STD_ERR_OK){
            EV_LOGGING(INTERFACE,ERR,"INTF-EV","Failed to update remote endpoint");
        }
    }

    return true;

}

t_std_error nas_vxlan_remote_endpoint_handler_register(void)
{

    cps_api_event_reg_t reg;
    memset(&reg,0,sizeof(reg));
    const uint_t NUM_EVENTS=1;

    cps_api_key_t keys[NUM_EVENTS];

    char buff[CPS_API_KEY_STR_MAX];

    if (!cps_api_key_from_attr_with_qual(&keys[0],
                BASE_MAC_TUNNEL_ENDPOINT_EVENT_OBJ, cps_api_qualifier_OBSERVED)) {
        EV_LOGGING(INTERFACE,ERR,"NAS-IF-REG","Could not translate %d to key %s",
            (int)(BASE_MAC_TUNNEL_ENDPOINT_EVENT_OBJ),cps_api_key_print(&keys[0],buff,sizeof(buff)-1));
        return STD_ERR(INTERFACE,FAIL,0);
    }
    EV_LOG(INFO, INTERFACE,0,"NAS-IF-REG", "Registered for interface events with key %s",
                    cps_api_key_print(&keys[0],buff,sizeof(buff)-1));

    reg.number_of_objects = NUM_EVENTS;
    reg.objects = keys;
    if (cps_api_event_thread_reg(&reg,nas_vxlan_remote_endpoint_handler_cb,NULL)!=cps_api_ret_code_OK) {
        return STD_ERR(INTERFACE,FAIL,0);
    }
    return STD_ERR_OK;
}
