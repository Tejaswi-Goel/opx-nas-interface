#!/usr/bin/python
# Copyright (c) 2017 Dell Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may
# not use this file except in compliance with the License. You may obtain
# a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
#
# THIS CODE IS PROVIDED ON AN *AS IS* BASIS, WITHOUT WARRANTIES OR
# CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT
# LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS
# FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.
#
# See the Apache Version 2.0 License for specific language governing
# permissions and limitations under the License.

import cps
import cps_object
import nas_os_if_utils as nas_if
import nas_mac_addr_utils as ma
import nas_if_config_obj as if_config
import nas_common_header as nas_comm

import logging
import time
import systemd.daemon
import threading

mac_attr_name = 'dell-if/if/interfaces/interface/phys-address'
_fp_port_key = cps.key_from_name('target','base-if-phy/front-panel-port')

def sigterm_hdlr(signum, frame):
    global shutdwn
    shutdwn = True

def get_mac_rpc_cb(methods, params):
    if params['operation'] != 'rpc':
        nas_if.log_err('Operation %s not supported' % params['operation'])
        return False
    cps_obj = cps_object.CPSObject(obj = params['change'])

    if_type = if_config.get_intf_type(cps_obj)
    if if_type == 'loopback' or if_type == 'management':
        nas_if.log_err('Interface type %s not supported' % if_type)
        return False
    with fp_lock:
        param_list = ma.get_alloc_mac_addr_params(if_type, cps_obj, fp_cache)
        if param_list is None:
            nas_if.log_err('No enough attributes in input object to get mac address')
            return False

    mac_addr = ma.if_get_mac_addr(**param_list)
    if mac_addr is None or len(mac_addr) == 0:
        nas_if.log_err('Failed to get mac address')
        return False
    cps_obj.add_attr(mac_attr_name, mac_addr)

    params['change'] = cps_obj.get()
    return True

def get_mac_cb(methods, params):
    try:
        return get_mac_rpc_cb(methods, params)
    except Exception:
        logging.exception('logical interface error')
    return False

def _update_fp(fp_obj):
    fr_port = nas_if.get_cps_attr(fp_obj, 'front-panel-port')
    if fr_port is None:
        nas_if.log_info('No front-panel-port found in event obj')
        nas_if.log_info('Event obj: %s' % str(fp_obj.get()))
        return
    if fr_port not in fp_cache:
        nas_if.log_err('Front panel port %d not in cache' % fr_port)
        return
    br_mode = nas_if.get_cps_attr(fp_obj, 'breakout-mode')
    if br_mode is None:
        nas_if.log_err('No breakout mode found in event obj')
        return
    cached_br_mode = nas_if.get_cps_attr(fp_cache[fr_port], 'breakout-mode')
    if br_mode != cached_br_mode:
        nas_if.log_info('Change breakout mode of fp port %d to %d' % (fr_port, br_mode))
        with fp_lock:
            fp_cache[fr_port].add_attr('breakout-mode', br_mode)

def monitor_fp_event():
    handle = cps.event_connect()
    cps.event_register(handle, _fp_port_key)
    while True:
        o = cps.event_wait(handle)
        obj = cps_object.CPSObject(obj = o)
        _update_fp(obj)

class fpMonitorThread(threading.Thread):
    def __init__(self, thread_id, name):
        threading.Thread.__init__(self)
        self.threadID = thread_id
        self.name = name
    def run(self):
        monitor_fp_event()
    def __str__(self):
        return ' %s %d ' % (self.name, self.threadID)

if __name__ == '__main__':

    shutdwn = False

    # Install signal handlers.
    import signal
    signal.signal(signal.SIGTERM, sigterm_hdlr)

    # Initialization complete
    # Notify systemd: Daemon is ready
    systemd.daemon.notify("READY=1")

    # Wait for base MAC address to be ready. the script will wait until
    # chassis object is registered.
    srv_chk_cnt = 0
    srv_chk_rate = 10
    chassis_key = cps.key_from_name('observed','base-pas/chassis')
    while cps.enabled(chassis_key)  == False:
        #wait for chassis object to be ready
        if srv_chk_cnt % srv_chk_rate == 0:
            nas_if.log_err('MAC address config: Base MAC address is not yet ready')
        time.sleep(1)
        srv_chk_cnt += 1
    nas_if.log_info('Base MAC address service is ready after checking %d times' % srv_chk_cnt)

    srv_chk_cnt = 0
    while cps.enabled(nas_comm.get_value(nas_comm.keys_id, 'physical_key'))  == False:
        if srv_chk_cnt % srv_chk_rate == 0:
            nas_if.log_info('MAC address config: Physical port service is not ready')
        time.sleep(1)
        srv_chk_cnt += 1
    nas_if.log_info('Physical port service is ready after checking %d times' % srv_chk_cnt)

    srv_chk_cnt = 0
    while cps.enabled(nas_comm.get_value(nas_comm.keys_id, 'fp_key'))  == False:
        if srv_chk_cnt % srv_chk_rate == 0:
            nas_if.log_info('MAC address config: Front panel port service is not ready')
        time.sleep(1)
        srv_chk_cnt += 1
    nas_if.log_info('Front panel port service is ready after checking %d times' % srv_chk_cnt)

    # Register MAc address allocation handler
    get_mac_hdl = cps.obj_init()

    d = {}
    d['transaction'] = get_mac_cb
    cps.obj_register(get_mac_hdl, nas_comm.get_value(nas_comm.keys_id, 'get_mac_key'), d)

    fp_cache = nas_if.FpPortCache()
    fp_thread = fpMonitorThread(1, 'Front panel port monitor thread')
    fp_thread.daemon = True
    fp_thread.start()

    # Lock to protect front panel port cache updated by thread
    fp_lock = threading.Lock()

    # Wait until a signal is received
    while False == shutdwn:
        signal.pause()

    systemd.daemon.notify("STOPPING=1")
    #Cleanup code here

    # No need to specifically call sys.exit(0).
    # That's the default behavior in Python.
