
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
 * filename: nas_interface_1q_bridge.cpp
 */

#include "nas_int_utils.h"
#include "bridge/nas_interface_1q_bridge.h"

const static int MAX_CPS_MSG_BUFF=10000;

/************************** PROTECTED ******************************/
/** NPU Implementation */
/** Virtual: Create/Delete a bridge in the NPU */
t_std_error NAS_DOT1Q_BRIDGE::nas_bridge_npu_create()
{
    t_std_error rc = STD_ERR_OK;
    /** Do nothing or VLAN is created when first tagged memeber is added */
    hal_vlan_id_t vlan_id = bridge_vlan_id;
    if (vlan_id == NAS_VLAN_ID_INVALID) {
        EV_LOGGING(INTERFACE, INFO, "BRIDGE-DOT-1Q", "No VLAN ID associated to the bridge %s",
                bridge_name.c_str());
        return STD_ERR_OK;
    }
    EV_LOGGING(INTERFACE, INFO, "DOT-1Q", "Creating VLAN %d in NPU %d", vlan_id, npu_id);
    /* TODO  NAS-L3 may need to be informed about the vlan creation */
    if ((rc = ndi_create_vlan(npu_id, bridge_vlan_id)) != STD_ERR_OK) {
        EV_LOGGING(INTERFACE, ERR, "DOT-1Q", "Failure Creating VLAN %d in NPU %d", vlan_id, npu_id);
        return rc;
    }
    if ((rc = nas_bridge_intf_cntrl_block_register(HAL_INTF_OP_REG)) != STD_ERR_OK) {
        EV_LOGGING(INTERFACE, ERR, "DOT-1Q", "Failure registering VLAN %d in cntrl block %s",
                         vlan_id, bridge_name.c_str());
        return rc;
    }
    hal_ifindex_t br_index = get_bridge_intf_index();
    if (nas_intf_handle_intf_mode_change(br_index, BASE_IF_MODE_MODE_NONE) == false) {
         EV_LOGGING(INTERFACE, DEBUG, "NAS-CPS-LAG",
                "Update to NAS-L3 about interface mode change failed(%d)", br_index);
    }
    return STD_ERR_OK;
}

t_std_error NAS_DOT1Q_BRIDGE::nas_bridge_npu_delete() {


    /** Delete VLAN in the NPU */
    EV_LOGGING(INTERFACE,NOTICE,"NAS-INT", " Bridge delete from NPU call");
    hal_vlan_id_t vlan_id = bridge_vlan_id;
    if((vlan_id != NAS_VLAN_ID_INVALID) && (vlan_id != SYSTEM_DEFAULT_VLAN)) {
        if(ndi_delete_vlan(npu_id, vlan_id) != STD_ERR_OK) {
            EV_LOGGING(INTERFACE, ERR, "DOT-1Q", "Error deleting vlan %d for bridge %d", vlan_id, if_index);
            return STD_ERR(INTERFACE,FAIL, 0);
        }
        t_std_error rc = STD_ERR_OK;
        if ((rc = nas_bridge_intf_cntrl_block_register(HAL_INTF_OP_DEREG)) != STD_ERR_OK) {
            EV_LOGGING(INTERFACE, ERR, "DOT-1Q", "Failure registering VLAN %d in cntrl block %s",
                             vlan_id, bridge_name.c_str());
            return rc;
        }
    }
    bridge_vlan_id = NAS_VLAN_ID_INVALID;

    /** Delete all vlan members in NPU */
    for (memberlist_t::iterator it = tagged_members.begin() ; it != tagged_members.end(); ++it) {
        EV_LOGGING(INTERFACE, DEBUG, "DOT-1Q", "Found intf %s for deletion", *it->c_str());
    }

    EV_LOGGING(INTERFACE,NOTICE,"NAS-INT", " Bridge delete from NPU successful");
    return STD_ERR_OK;
}
// TODO Following functions can be moved to utils file
static t_std_error _nas_npu_add_remove_port_to_vlan(ndi_port_t *ndi_port, hal_vlan_id_t vlan_id,
                                nas_port_mode_t port_mode, bool add) {

    t_std_error rc = STD_ERR_OK;
    ndi_port_list_t port_list = {1, ndi_port};
    ndi_port_list_t *t_list = NULL, *ut_list = NULL;
    (port_mode == NAS_PORT_UNTAGGED) ?
        (ut_list = &port_list) : (t_list = &port_list);
    if (add) {
        rc = ndi_add_ports_to_vlan(ndi_port->npu_id, vlan_id, t_list, ut_list);
        if (port_mode == NAS_PORT_UNTAGGED) {
             ndi_set_port_vid(ndi_port->npu_id, ndi_port->npu_port, vlan_id);
        }
    } else {
        rc = ndi_del_ports_from_vlan(ndi_port->npu_id, vlan_id, t_list, ut_list);
    }
    return rc;
}
static t_std_error _nas_npu_add_remove_lag_to_vlan(ndi_obj_id_t *lag_id, hal_vlan_id_t vlan_id,
                                nas_port_mode_t port_mode, bool add) {
    t_std_error rc = STD_ERR_OK;
    ndi_obj_id_t *t_list = NULL, *ut_list = NULL;
    size_t tag_cnt =0, untag_cnt=0;

    (port_mode == NAS_PORT_UNTAGGED) ?  (ut_list = lag_id, untag_cnt=1) :
                                  (t_list = lag_id, tag_cnt=1);
   if (add) {
        rc  = ndi_add_lag_to_vlan(0, vlan_id, t_list, tag_cnt, ut_list, untag_cnt);
        if (port_mode == NAS_PORT_UNTAGGED) {
            ndi_set_lag_pvid(0, *lag_id, vlan_id);
        }
    } else {
        rc = ndi_del_lag_from_vlan(0, vlan_id, t_list, tag_cnt, ut_list, untag_cnt);
    }
   return rc;
}

t_std_error NAS_DOT1Q_BRIDGE::nas_bridge_npu_update_all_untagged_members(void)
{
    t_std_error rc = STD_ERR_OK;
    /*  check if vlan id is not zero */
    if (bridge_vlan_id == 0) {
        return STD_ERR(INTERFACE,FAIL, rc);
    }
    for (auto it = untagged_members.begin(); it != untagged_members.end(); ++it) {
        interface_ctrl_t intf_ctrl;
        memset(&intf_ctrl, 0, sizeof(interface_ctrl_t));
        intf_ctrl.q_type = HAL_INTF_INFO_FROM_IF_NAME;
        safestrncpy(intf_ctrl.if_name, &(*it->c_str()), sizeof(intf_ctrl.if_name));
        if((rc= dn_hal_get_interface_info(&intf_ctrl)) != STD_ERR_OK) {
            EV_LOGGING(INTERFACE, ERR, "NAS-BRIDGE",
                   "Interface %s returned error %d", intf_ctrl.if_name, rc);
            return STD_ERR(INTERFACE,FAIL, rc);
        }

    /*  Change the mode to L2 if it is not already L2 */
        nas_port_mode_t port_mode = NAS_PORT_UNTAGGED;
        if_master_info_t master_info = { nas_int_type_VLAN, port_mode, intf_ctrl.if_index };
        BASE_IF_MODE_t new_intf_mode;
        bool mode_change = false;
        if(!nas_intf_add_master(intf_ctrl.if_index, master_info, &new_intf_mode, &mode_change)){
            EV_LOGGING(INTERFACE,ERR,"NAS-BRIDGE","Failed to add master %s for memeber port %d",
                                    bridge_name.c_str(), intf_ctrl.if_index);
        }
        if (mode_change) {
            if (nas_intf_handle_intf_mode_change(intf_ctrl.if_index, new_intf_mode) == false) {
                EV_LOGGING(INTERFACE,ERR,"NAS-BRIDGE",
                        "Update to NAS-L3 about interface mode change failed(%s)", intf_ctrl.if_name);
                // TODO no need to return failure
            }
        }
        if (intf_ctrl.int_type == nas_int_type_LAG) {
            rc = _nas_npu_add_remove_lag_to_vlan(&intf_ctrl.lag_id, intf_ctrl.vlan_id, NAS_PORT_UNTAGGED, true);
        } else if ((intf_ctrl.int_type == nas_int_type_PORT) &&
            !(nas_is_virtual_port(intf_ctrl.if_index))) {
            /*  TODO Call NDI function to Add/deleting phy port to the .1D bridge */
            ndi_port_t ndi_port = {intf_ctrl.npu_id, intf_ctrl.port_id};
            rc = _nas_npu_add_remove_port_to_vlan(&ndi_port, intf_ctrl.vlan_id, NAS_PORT_UNTAGGED, true);
        }
    }
    return STD_ERR_OK;
}


t_std_error NAS_DOT1Q_BRIDGE::nas_bridge_npu_add_remove_member(std::string &mem_name,
                                                    nas_int_type_t mem_type, bool add_member)
{
    /** STEPS:
     * 1. find out the type of the member
     * 2. Verify if the member is not already present in the member list
     * 3. it could be vlan sub interface ( tagged) or untagged PHY or LAG
     * If vlan sub interface then
     * Get the VLAN ID from the vlan interface
     * if VLAN ID is not same as bridge vlan id ( if bridge vlan is not 0) then /
     *   do not do anything and return
     * else
     *   if add then
     *      Get physical port info
     *      IF LAG then get lag id
     *       else get ndiport id from interface control block
     *      if bridge vlan is 0
     *       set the bridge vlan id
     *       Create vlan in the NPU
     *       Add all untagged ports to the vlan in the NPU
     *      set call ndi API to add the tagged member
     *
     *   else if remove then
     *      if member Does not exists inthe member list
     *        return
     *
     *      Get physical port information from object
     *      If LAG then get LAG information lag ID
     *      else if phy port then get ndi port and interface index from control block
     *      Call NDI function to remove the member from the vlan if not virtual interface.
     *      Publish the event of member addition/deletion
     *
     */
    t_std_error rc = STD_ERR_OK;
    hal_vlan_id_t vlan_id = 0;
    nas_port_mode_t port_mode;
    interface_ctrl_t intf_ctrl;
    memset(&intf_ctrl, 0, sizeof(interface_ctrl_t));
    intf_ctrl.q_type = HAL_INTF_INFO_FROM_IF_NAME;

    if ( mem_type == nas_int_type_VLANSUB_INTF) {
    /** If interface type is VLAN Subinterface
     *   retrieve physical interface (phy or lag) and vlan ID from vlan subinterface object,
     *    add membership to 1d bridge
     * */
        NAS_VLAN_INTERFACE *vlan_intf_obj = nullptr;
        vlan_intf_obj = dynamic_cast<NAS_VLAN_INTERFACE *>(nas_interface_map_obj_get(mem_name));
        if (vlan_intf_obj == nullptr) {
            return STD_ERR(INTERFACE,FAIL, rc);
        }
        vlan_id  = vlan_intf_obj->vlan_id;
        if (bridge_vlan_id == 0) {
            bridge_vlan_id = vlan_id;
            /*  then create vlan in the  NPU and add all tagged members */
            if ((rc = nas_bridge_npu_create()) != STD_ERR_OK) {
                EV_LOGGING(INTERFACE, ERR, "NAS-BRIDGE",
                   "Failed to create VLAN %d in the NPU for bridge %s : Error %d ",
                         vlan_id, bridge_name.c_str(), rc);
                return rc;
            }
            if ((rc = nas_bridge_npu_update_all_untagged_members()) != STD_ERR_OK) {
                EV_LOGGING(INTERFACE, ERR, "NAS-BRIDGE", "Failed to update untagged vlan members in the NPU for bridge %s",
                        bridge_name.c_str());
                return STD_ERR(INTERFACE, FAIL, 0);
            }
        } else if (bridge_vlan_id != vlan_id) {
            EV_LOGGING(INTERFACE,ERR,"NAS-BRIDGE", "Member addition Failed for bridge %s member %s due to",
                                 "VLAN ID mismatch  %d and  %d",
                                 bridge_name.c_str(), mem_name.c_str(), bridge_vlan_id, vlan_id);
            return STD_ERR(INTERFACE, FAIL, 0);
        }
        /*  member Vlan Id and bridge vlan Id are same now */
        std::string phy_intf = vlan_intf_obj->parent_intf_name;
        safestrncpy(intf_ctrl.if_name, phy_intf.c_str(), sizeof(intf_ctrl.if_name));
        port_mode = NAS_PORT_TAGGED;

    } else if ((mem_type == nas_int_type_PORT) || (mem_type = nas_int_type_LAG)) {
        /** Else If interface type is phy/FC or lag (untagged),
         *     set mode of phy/LAG to L2 in NAS-L3,
         *     use default/reserved vlan id
         *     add interface to the bridge if interface not virtual)
         *
         * */
        if (bridge_vlan_id == 0) {
            /*  TODO Add in the untagged member list and return */
            /*  publish msg */
            return STD_ERR_OK;
        }
        safestrncpy(intf_ctrl.if_name, mem_name.c_str(), sizeof(intf_ctrl.if_name));
        port_mode = NAS_PORT_UNTAGGED;
        vlan_id = bridge_vlan_id;
    } else {
        /*  Unknown or unsupported member type. Return  */
        /*  TODO How about MGMT type member ?? */
        EV_LOGGING(INTERFACE,ERR,"NAS-BRIDGE", "Member addition Failed for bridge %s member %d due to",
                                 "Invalid Member type %d",  bridge_name.c_str(), intf_ctrl.if_index, mem_type);
        return STD_ERR(INTERFACE, FAIL, 0);
    }
    if((rc= dn_hal_get_interface_info(&intf_ctrl)) != STD_ERR_OK) {
        EV_LOGGING(INTERFACE, ERR, "NAS-BRIDGE",
                   "Interface %s returned error %d", intf_ctrl.if_name, rc);
        return STD_ERR(INTERFACE,FAIL, rc);
    }

    /*  Change the mode to L2 if it is not already L2 */
    if_master_info_t master_info = { nas_int_type_VLAN, port_mode, intf_ctrl.if_index };
    BASE_IF_MODE_t new_intf_mode;
    bool mode_change = false;
    if (add_member) /*  Add member case */{
        if(!nas_intf_add_master(intf_ctrl.if_index, master_info, &new_intf_mode, &mode_change)){
            EV_LOGGING(INTERFACE,ERR,"NAS-BRIDGE","Failed to add master %s for memeber port %d",
                                        bridge_name.c_str(), intf_ctrl.if_index);
        }
    } else /*  delete member case */{
        if(!nas_intf_del_master(intf_ctrl.if_index, master_info, &new_intf_mode, &mode_change)){
            EV_LOGGING(INTERFACE,ERR,"NAS-BRIDGE","Failed to delete master %s for memeber port %d",
                                        bridge_name.c_str(), intf_ctrl.if_index);
        }
        // Clean up l2mc mebership in case of delete member
        if(!nas_intf_cleanup_l2mc_config(intf_ctrl.if_index, vlan_id)) {
        EV_LOGGING(INTERFACE, ERR, "NAS-Vlan",
               "Error cleaning L2MC membership for interface %s", intf_ctrl.if_name);
        }
    }
    if (mode_change) {
        if (nas_intf_handle_intf_mode_change(intf_ctrl.if_index, new_intf_mode) == false) {
            EV_LOGGING(INTERFACE,ERR,"NAS-BRIDGE", "Update to NAS-L3 about interface mode change failed(%s)",
                    intf_ctrl.if_name);
            // TODO no need to return failure
        }
    }
    // call nas_intf_cleanup_l2mc_config(p_link_node->ifindex,  p_bridge->vlan_id)) if 1Q mode

    /*  If bridge vlan ID is 0 and set tge vlan_id to the bridge */
    if (intf_ctrl.int_type == nas_int_type_LAG) {
        rc = _nas_npu_add_remove_lag_to_vlan(&intf_ctrl.lag_id, vlan_id, port_mode, add_member);
    } else if ((intf_ctrl.int_type == nas_int_type_PORT) &&
        !(nas_is_virtual_port(intf_ctrl.if_index))) {
        /*  TODO Call NDI function to Add/deleting phy port to the .1D bridge */
        ndi_port_t ndi_port = {intf_ctrl.npu_id, intf_ctrl.port_id};
        rc = _nas_npu_add_remove_port_to_vlan(&ndi_port, vlan_id, port_mode, add_member);
    }
    if (rc != STD_ERR_OK) {
        EV_LOGGING(INTERFACE,ERR,"NAS-BRIDGE","Failed to add member %s to the Vlan %d in the NPU",
                intf_ctrl.if_name, vlan_id);
        return rc;
    }

    /*  Update bridge membership */
    nas_bridge_update_member_list(mem_name, port_mode, add_member);
    return STD_ERR_OK;
}

/*  Add a member to the bridge */
t_std_error NAS_DOT1Q_BRIDGE::nas_bridge_add_remove_member(std::string & mem_name, nas_port_mode_t port_mode, bool add)
{
    // first add in the kernel and then add in the npu
    t_std_error  rc = STD_ERR_OK;

    if ((rc = nas_bridge_os_add_remove_member(mem_name, port_mode, add)) != STD_ERR_OK) {
        return rc;
    }
    /*  TODO IF port does not exist then add in the interface map */
    nas_int_type_t mem_type ;
    if (port_mode == NAS_PORT_TAGGED) {
        mem_type = nas_int_type_VLANSUB_INTF;
    } else {
        if ((rc = nas_get_int_name_type(mem_name.c_str(), &mem_type)) != STD_ERR_OK) {
            EV_LOGGING(INTERFACE,ERR, "NAS-BRIDGE", " NAS OS L2 PORT Event: Failed to get member type %s ", mem_name.c_str());
            return rc ;
        }
    }
    if ((rc = nas_bridge_npu_add_remove_member(mem_name, mem_type, add)) != STD_ERR_OK) {

        nas_bridge_os_add_remove_member(mem_name, port_mode, !add);
        EV_LOGGING(INTERFACE,ERR,"INT-DB-GET","Failed to %s member %s to/from bridge %s",
                            (add ? "add":"delete"), mem_name.c_str(), get_bridge_name());
        return rc;
    }
    return STD_ERR_OK;
}

t_std_error NAS_DOT1Q_BRIDGE::nas_bridge_add_remove_memberlist(memberlist_t & m_list, nas_port_mode_t port_mode, bool add)
{
    t_std_error  rc = STD_ERR_OK;
    memberlist_t processed_mem_list;
    for (auto mem : m_list) {
        if ((rc = nas_bridge_add_remove_member(mem, port_mode, add)) != STD_ERR_OK) {
            break;
        }
        // Save processed member in a list
        processed_mem_list.insert(mem);
    }
    if (rc != STD_ERR_OK) {
        // IF any failure then rollback the added or removed members
        for (auto mem : processed_mem_list) {
            if ((rc = nas_bridge_add_remove_member(mem, port_mode, !add)) != STD_ERR_OK) {
                EV_LOGGING(INTERFACE,ERR,"INT-DB-GET","Failed to rollback member %s update", mem.c_str());
            }
        }
    }
    return rc;
}
t_std_error NAS_DOT1Q_BRIDGE::nas_bridge_associate_npu_port(std::string &mem_name, ndi_port_t *ndi_port, nas_port_mode_t port_mode, bool associate)
{

    // TODO check if mem_name exists in the member list
    return _nas_npu_add_remove_port_to_vlan(ndi_port, nas_bridge_vlan_id_get(), port_mode, associate);

}
t_std_error NAS_DOT1Q_BRIDGE::nas_bridge_intf_cntrl_block_register(hal_intf_reg_op_type_t op) {

    interface_ctrl_t details;

    EV_LOGGING(INTERFACE, INFO ,"NAS-BRIDGE", " %s Vlan %d %d with ifCntrl name %s",
            (op == HAL_INTF_OP_REG) ? "Registering" : "Deregistering",
           if_index, bridge_vlan_id, bridge_name.c_str());

    memset(&details,0,sizeof(details));
    details.q_type = HAL_INTF_INFO_FROM_IF_NAME;
    details.if_index = if_index;
    safestrncpy(details.if_name,bridge_name.c_str(), sizeof(details.if_name));
    details.vlan_id = bridge_vlan_id;
    details.int_type = nas_int_type_VLAN;
    details.int_sub_type = bridge_sub_type;

    if ((op == HAL_INTF_OP_DEREG) && ((dn_hal_get_interface_info(&details)) != STD_ERR_OK)) {
        EV_LOGGING(INTERFACE,ERR,"NAS-Vlan", "VLAN %d %s Not registered with ifCntrl ",
               if_index, bridge_name.c_str());
        return STD_ERR_OK;
    }

    if(op == HAL_INTF_OP_DEREG && details.int_type != nas_int_type_VLAN){
        EV_LOGGING(INTERFACE,INFO,"NAS-VLAN","Bridge %s is registered as %d type. Can't deregister "
                    "it using type %d",bridge_name.c_str(),details.int_type,nas_int_type_VLAN);
        return STD_ERR_OK;
    }
    if (dn_hal_if_register(op, &details)!=STD_ERR_OK) {
        EV_LOGGING(INTERFACE,ERR,"NAS-VLAN", "Failed to (de)register VLAN %d %s with ifCntrl block op=%d ",
               if_index, bridge_name.c_str(), op);
        return STD_ERR(INTERFACE,FAIL,0);
    }
    return STD_ERR_OK;
}

cps_api_return_code_t NAS_DOT1Q_BRIDGE::nas_bridge_fill_info(cps_api_object_t obj)
{
    if (get_bridge_model() == INT_VLAN_MODEL) {
        cps_api_object_attr_add_u32(obj, DELL_IF_IF_INTERFACES_INTERFACE_VLAN_TYPE,
                                 nas_bridge_sub_type_get());
        cps_api_object_attr_add_u32(obj, BASE_IF_VLAN_IF_INTERFACES_INTERFACE_ID, nas_bridge_vlan_id_get());
    }
    return nas_bridge_fill_com_info(obj);

}

t_std_error NAS_DOT1Q_BRIDGE::nas_bridge_set_learning_disable(bool disable){

    if(ndi_set_vlan_learning(0, bridge_vlan_id, disable) != STD_ERR_OK) {
        EV_LOGGING(INTERFACE, ERR ,"NAS-Vlan", "Error setting learning disable state for VLAN %s",
               bridge_name.c_str());
            return STD_ERR(INTERFACE,FAIL,0);
    }

    learning_disable = disable;

    return STD_ERR_OK;
}
