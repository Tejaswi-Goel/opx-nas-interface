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
 * nas_int_common_obj.cpp
 *
 */


#include "dell-base-if.h"
#include "dell-base-if-vlan.h"
#include "iana-if-type.h"

#include "std_error_codes.h"
#include "cps_api_operation.h"
#include "cps_api_object_key.h"

#include "event_log.h"
#include "cps_class_map.h"
#include "cps_api_db_interface.h"
#include <unordered_map>

#include "interface_obj.h"

#include "nas_int_utils.h"

typedef struct _intf_obj_handler_s {
    cps_rdfn obj_rd;
    cps_wrfn obj_wr;
} intf_obj_handler_t;

#define INTF_TYPE_MAX_LEN 256
#define NUM_INT_CPS_API_THREAD 1

static cps_api_operation_handle_t nas_if_stat_handle;

// get/set handlers based on category ( INTF/INTF_STATE/INTF_STATISTICS) and intf type (PHY/VLAN/LAG)
static  auto _intf_handlers = new std::unordered_map <nas_int_type_t, intf_obj_handler_t *, std::hash<int>> [obj_INTF_MAX];

static t_std_error _if_type_from_if_index_or_name(obj_intf_cat_t obj_cat, cps_api_object_t obj,
                                                  nas_int_type_t *type) {

    interface_ctrl_t if_info;
    cps_api_object_attr_t _ifix = cps_api_object_attr_get(obj, (obj_cat == obj_INTF) ?
                                                (uint) DELL_BASE_IF_CMN_IF_INTERFACES_INTERFACE_IF_INDEX :
                                                (uint)IF_INTERFACES_STATE_INTERFACE_IF_INDEX);
    memset(&if_info,0,sizeof(if_info));
        /*  check if if_index is present */
    if (_ifix != nullptr)  {
        if_info.if_index = cps_api_object_attr_data_u32(_ifix);
        if_info.q_type = HAL_INTF_INFO_FROM_IF;
    } else { /*  check for interface name */
        cps_api_object_attr_t _name = cps_api_get_key_data(obj, (obj_cat == obj_INTF) ?
                                                (uint)IF_INTERFACES_INTERFACE_NAME :
                                                (uint)IF_INTERFACES_STATE_INTERFACE_NAME);

        if (_name == nullptr) return STD_ERR(INTERFACE,PARAM,0);

        strncpy(if_info.if_name,(const char *)cps_api_object_attr_data_bin(_name),sizeof(if_info.if_name)-1);
        if_info.q_type = HAL_INTF_INFO_FROM_IF_NAME;
    }

    if (dn_hal_get_interface_info(&if_info)!= STD_ERR_OK) return STD_ERR(INTERFACE,PARAM,0);

    *type = if_info.int_type;
    return STD_ERR_OK;
}

t_std_error nas_intf_db_obj_get(const char * intf_name,cps_api_attr_id_t id, cps_api_qualifier_t cat,
                            cps_api_object_t obj){

    if(intf_name == nullptr){
        EV_LOGGING(INTERFACE,ERR,"INT-DB-GET","Null intf name passed to retreive object from db");
        return STD_ERR(INTERFACE,PARAM,0);
    }

    if(obj == nullptr){
        EV_LOGGING(INTERFACE,ERR,"INT-DB-GET","Null object passed to retreive from db");
        return STD_ERR(INTERFACE,PARAM,0);
    }

    cps_api_object_guard _og(cps_api_object_create());
    if(_og.valid()){
        cps_api_key_from_attr_with_qual(cps_api_object_key(_og.get()),id,cat);
        cps_api_set_key_data(_og.get(),IF_INTERFACES_INTERFACE_NAME,cps_api_object_ATTR_T_BIN,intf_name,strlen(intf_name)+1);
        cps_api_object_list_guard lst(cps_api_object_list_create());
        if (cps_api_db_get(_og.get(),lst.get())==cps_api_ret_code_OK) {
            size_t len = cps_api_object_list_size(lst.get());

            if(len){
                // Get should return only one object matching to the interface name
                cps_api_object_clone(obj,cps_api_object_list_get(lst.get(),0));
                return STD_ERR_OK;
            }
        }
    }

    EV_LOGGING(INTERFACE,ERR,"NAS-INT-SET","Failed to get stored configs for interface %s",intf_name);
    return cps_api_ret_code_ERR;
}

static void cps_api_object_list_swap(cps_api_object_list_t &a, cps_api_object_list_t &b) {
    cps_api_object_list_t t = a;
    a = b;
    b = t;
}

t_std_error _process_get_request(obj_intf_cat_t obj_cat, void * context, cps_api_get_params_t * param,
                                 size_t key_ix, nas_int_type_t type, cps_api_object_list_t list) {
    cps_api_attr_id_t _type_attr_id = (obj_cat == obj_INTF) ?
                       (cps_api_attr_id_t) IF_INTERFACES_INTERFACE_TYPE :
                       (cps_api_attr_id_t) IF_INTERFACES_STATE_INTERFACE_TYPE ;
    cps_api_object_t filt = cps_api_object_list_get(param->filters,key_ix);

    if ((_intf_handlers[obj_cat][type] == NULL) || (_intf_handlers[obj_cat][type]->obj_rd == NULL))  {
         EV_LOGGING(INTERFACE,ERR,"NAS-COM-INT-GET","Get request handler not present");
         return cps_api_ret_code_ERR;
    }

    char if_type[INTF_TYPE_MAX_LEN];
    if (!nas_to_ietf_if_type_get(type, if_type, sizeof(if_type))) {
        EV_LOGGING(INTERFACE, ERR, "NAS-COM-INT-GET", "Failed to get IETF type for NAS type %d",type);
        return cps_api_ret_code_ERR;
    }
    cps_api_object_attr_t _ietf_type_attr = cps_api_object_attr_get(filt, _type_attr_id);
    if(! _ietf_type_attr)  cps_api_object_attr_add(filt, _type_attr_id, if_type, strlen(if_type) + 1);

    cps_api_object_list_swap(param->list, list);
    t_std_error ret = _intf_handlers[obj_cat][type]->obj_rd(context, param, key_ix);
    cps_api_object_list_swap(param->list, list);

    if(ret == STD_ERR_OK) {
        // Merge the output of get handlers in the output param->list
        cps_api_object_list_merge(param->list, list);
    }
    cps_api_object_attr_delete(filt, _type_attr_id);
    cps_api_object_list_clear(list, true);
    return ret;
};

static cps_api_return_code_t _if_gen_interface_get(obj_intf_cat_t obj_cat, void * context,
                                             cps_api_get_params_t * param, size_t key_ix) {

    cps_api_object_t filt = cps_api_object_list_get(param->filters,key_ix);
    cps_api_attr_id_t _type_attr_id = (obj_cat == obj_INTF) ?
                       (cps_api_attr_id_t) IF_INTERFACES_INTERFACE_TYPE :
                       (cps_api_attr_id_t) IF_INTERFACES_STATE_INTERFACE_TYPE ;
    cps_api_object_list_guard list(cps_api_object_list_create());
    if(list.get() == NULL) {
        EV_LOGGING(INTERFACE,ERR,"NAS-COM-INT-GET","Can't create CPS Object output list");
        return cps_api_ret_code_ERR;
    }

    cps_api_object_attr_t ifix = cps_api_object_attr_get(filt, (obj_cat == obj_INTF) ?
                                        (cps_api_attr_id_t)DELL_BASE_IF_CMN_IF_INTERFACES_INTERFACE_IF_INDEX:
                                        (cps_api_attr_id_t)IF_INTERFACES_STATE_INTERFACE_IF_INDEX);
    cps_api_object_attr_t _name = cps_api_get_key_data(filt, (obj_cat == obj_INTF) ?
                                                     (cps_api_attr_id_t)IF_INTERFACES_INTERFACE_NAME :
                                                     (cps_api_attr_id_t)IF_INTERFACES_STATE_INTERFACE_NAME);
    cps_api_object_attr_t _ietf_type_attr = cps_api_object_attr_get(filt,
                                            _type_attr_id);
    nas_int_type_t input_type, type;
    if (_ietf_type_attr != nullptr) {
        const char *ietf_intf_type = (const char *)cps_api_object_attr_data_bin(_ietf_type_attr);
        if(!ietf_to_nas_if_type_get(ietf_intf_type, &input_type)) {
            EV_LOGGING(INTERFACE,ERR,"NAS-COM-INT-GET","Could not convert the %s type to nas if type",ietf_intf_type);
            return cps_api_ret_code_ERR; //Not supported interface type
        }
    }
    /* If this attribute is present in an object, then the caller would like the next object in lexicographic order*/
    bool is_get_next = cps_api_filter_is_getnext(filt);
    hal_ifindex_t index = 0, *current_ifindex = nullptr, next_ifindex;
    size_t count = 0;

    if(ifix != nullptr || _name != nullptr) {
        const char *name = nullptr;
        if(ifix) index = cps_api_object_attr_data_u32(ifix);
        if(_name) name = (const char *)cps_api_object_attr_data_bin(_name);
        nas_get_if_type_from_name_or_ifindex(name, &index, &type);
        count = 1 ;
    }

    //Support get exact filtered by vlan-id
    if (_ietf_type_attr != nullptr && input_type ==  nas_int_type_VLAN) {
        cps_api_object_attr_t vlan_id_attr = cps_api_object_attr_get(filt,
                                    BASE_IF_VLAN_IF_INTERFACES_INTERFACE_ID);
        if(vlan_id_attr) {
            count = 1;
            type = input_type;
        }
    }


    /*Number of entries to retrieve at one time to support the concept of range along with the get next attribute*/
    cps_api_filter_get_count(filt, &count);
    t_std_error ret, res;

    // Get Exact by interface name or ifindex
    if(count == 1 && !is_get_next) {
        res = _process_get_request(obj_cat, context, param, key_ix, type, list.get());
        return res;
    }

    if(is_get_next && (ifix != nullptr || _name != nullptr)) current_ifindex = &index;
    do {
        ret = dn_hal_get_next_ifindex(current_ifindex, &next_ifindex);
        if(ret == STD_ERR_OK && (nas_get_if_type_from_name_or_ifindex(nullptr, &next_ifindex, &type) == STD_ERR_OK)) {
          if(_ietf_type_attr == nullptr || type == input_type) {
            if(obj_cat == obj_INTF) {
              cps_api_object_attr_delete(filt, DELL_BASE_IF_CMN_IF_INTERFACES_INTERFACE_IF_INDEX);
              cps_api_object_attr_add_u32(filt,DELL_BASE_IF_CMN_IF_INTERFACES_INTERFACE_IF_INDEX, next_ifindex);
            } else {
              cps_api_object_attr_delete(filt, IF_INTERFACES_STATE_INTERFACE_IF_INDEX);
              cps_api_object_attr_delete(filt, DELL_BASE_IF_CMN_IF_INTERFACES_INTERFACE_IF_INDEX);
              cps_api_object_attr_add_u32(filt,IF_INTERFACES_STATE_INTERFACE_IF_INDEX, next_ifindex);
            }
            res = _process_get_request(obj_cat, context, param, key_ix, type, list.get());
            if(res == STD_ERR_OK) {
                // Get First
                if(ifix == nullptr && _name == nullptr && is_get_next) break;
                // Get Next with count
                if(count != 0) {
                    --count;
                    if(!count) break;
                }
            }
          }
        }
        current_ifindex = &next_ifindex;
    } while(ret == STD_ERR_OK);
    return cps_api_ret_code_OK;
}

static cps_api_return_code_t _if_gen_interface_set (obj_intf_cat_t obj_cat, void * context,
                                            cps_api_transaction_params_t * param,size_t ix) {
    nas_int_type_t  _type;

    cps_api_object_t obj = cps_api_object_list_get(param->change_list,ix);
    if (obj==nullptr) return cps_api_ret_code_ERR;

    cps_api_object_attr_t _ietf_type_attr = cps_api_object_attr_get(obj,
                                            IF_INTERFACES_INTERFACE_TYPE); //TODO only of obj_INTF
    if (_ietf_type_attr != nullptr) {
        const char *ietf_intf_type = (const char *)cps_api_object_attr_data_bin(_ietf_type_attr);
        if(ietf_to_nas_if_type_get(ietf_intf_type, &_type) == false) {
            EV_LOGGING(INTERFACE,ERR,"NAS-COM-INT-SET","Could not convert the %s type to nas if type",ietf_intf_type);
            return cps_api_ret_code_ERR;
        }
    } else {
        /*  extract type from if_name or if_index */
        if (_if_type_from_if_index_or_name(obj_cat, obj, &_type) != STD_ERR_OK) {
            EV_LOGGING(INTERFACE,ERR,"NAS-COM-INT-SET","No interface name or index passed to process "
                                                 "common interface set request");
            return cps_api_ret_code_ERR;
        }
    }
    auto iter = _intf_handlers[obj_cat].find(_type);
    if (iter == _intf_handlers[obj_cat].end()) {
        EV_LOGGING(INTERFACE,ERR,"NAS-COM-INT-SET","Set request handler was not registered for obj "
                                "Category %d and type %d", obj_cat, _type);
        return cps_api_ret_code_ERR; //Handler not registered for this interface type
    }
    if ((_intf_handlers[obj_cat][_type] == NULL) || (_intf_handlers[obj_cat][_type]->obj_wr == NULL)) {
        EV_LOGGING(INTERFACE,ERR,"NAS-COM-INT-SET","No CPS Set handler for interface cat %d and type %d",obj_cat,_type);
        return cps_api_ret_code_ERR; //Handler not present for this interface type
    }

    EV_LOGGING(INTERFACE,INFO,"NAS-COM-INT-SET","Interface Set request received for obj category %d type %d.",obj_cat, _type);
    return(_intf_handlers[obj_cat][_type]->obj_wr(context, param,ix));

}

// Interface object get set handlers
static cps_api_return_code_t _if_interface_get(void * context, cps_api_get_params_t * param, size_t key_ix) {
    return(_if_gen_interface_get(obj_INTF, context, param, key_ix));
}

static cps_api_return_code_t _if_interface_set (void * context,
                                            cps_api_transaction_params_t * param,size_t ix) {
    return(_if_gen_interface_set (obj_INTF, context, param, ix));
}

// Interface state object get set handlers
static cps_api_return_code_t _if_interface_state_get(void * context, cps_api_get_params_t * param,
        size_t key_ix) {
    return(_if_gen_interface_get(obj_INTF_STATE, context, param, key_ix));
}

static cps_api_return_code_t _if_interface_state_set (void * context, cps_api_transaction_params_t * param,size_t ix) {
    return(_if_gen_interface_set (obj_INTF_STATE, context, param, ix));
}

// Interface statistics object get set handlers
static cps_api_return_code_t _if_interface_state_statistics_get(void * context, cps_api_get_params_t * param,
        size_t key_ix) {
    return(_if_gen_interface_get(obj_INTF_STATISTICS, context, param, key_ix));
}

static cps_api_return_code_t _if_interface_state_statistics_set (void * context, cps_api_transaction_params_t * param,size_t ix) {
    return(_if_gen_interface_set (obj_INTF_STATISTICS, context, param, ix));
}


t_std_error intf_obj_handler_registration(obj_intf_cat_t obj_cat, nas_int_type_t intf_type, cps_rdfn rd, cps_wrfn wr) {
    STD_ASSERT(rd); STD_ASSERT(wr);

    intf_obj_handler_t  *h = new intf_obj_handler_t ;
    if (h == NULL) return STD_ERR(INTERFACE,FAIL,0); // TODO error type
    h->obj_rd = rd;
    h->obj_wr = wr;

    _intf_handlers[obj_cat][intf_type] =  h;
    return STD_ERR_OK;
}

static t_std_error _reg_module(cps_api_operation_handle_t handle, cps_api_attr_id_t id,
                               cps_api_qualifier_t qual, cps_rdfn rd, cps_wrfn wr) {
    cps_api_registration_functions_t f;
    memset(&f,0,sizeof(f));

    char buff[CPS_API_KEY_STR_MAX];
    if (!cps_api_key_from_attr_with_qual(&f.key,id,qual)) {
        EV_LOGGING(INTERFACE,ERR,"NAS-IF-REG","Could not translate %d to key %s",
            (int)(id),cps_api_key_print(&f.key,buff,sizeof(buff)-1));
        return STD_ERR(INTERFACE,FAIL,0);
    }

    f.handle = handle;
    f._read_function = rd;
    f._write_function = wr;

    if (cps_api_register(&f)!=cps_api_ret_code_OK) {
        return STD_ERR(INTERFACE,FAIL,0);
    }

    return STD_ERR_OK;
}

t_std_error interface_obj_init(cps_api_operation_handle_t handle)  {

    t_std_error rc;
    if ((rc=_reg_module(handle,DELL_BASE_IF_CMN_IF_INTERFACES_INTERFACE_OBJ, cps_api_qualifier_TARGET,
            _if_interface_get,_if_interface_set))!=STD_ERR_OK) {
        return rc;
    }

    if ((rc=_reg_module(handle,DELL_BASE_IF_CMN_IF_INTERFACES_STATE_INTERFACE_OBJ, cps_api_qualifier_OBSERVED,
            _if_interface_state_get,_if_interface_state_set))!=STD_ERR_OK) {
        return rc;
    }

    //Create a handle for STATS objects
    if (cps_api_operation_subsystem_init(&nas_if_stat_handle,NUM_INT_CPS_API_THREAD)!=cps_api_ret_code_OK) {
        return STD_ERR(CPSNAS,FAIL,0);
    }

    if ((rc=_reg_module(nas_if_stat_handle,DELL_BASE_IF_CMN_IF_INTERFACES_STATE_INTERFACE_STATISTICS_OBJ,cps_api_qualifier_OBSERVED,
            _if_interface_state_statistics_get,_if_interface_state_statistics_set))!=STD_ERR_OK) {
        return rc;
    }

    return STD_ERR_OK;
}
