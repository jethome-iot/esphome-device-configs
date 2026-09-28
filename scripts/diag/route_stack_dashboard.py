#!/usr/bin/env python3
"""Per-route worst-case httpd-task stack for components/web_device_dashboard.

Frames come from the corpus callgraph (each function's Xtensa `entry` immediate + any extra
explicit sp adjustment), cross-checked against the -fstack-usage corpus.  Indirect edges that
the automated graph cannot see (std::function, AsyncWebHandler vtables) are spliced in here,
each one read out of the disassembly.
"""
import json,sys,argparse
SP='/tmp/claude-1000/-home-alexey-dev-esphome-device-configs/bf4965c5-a19f-4694-a820-14d815214413/scratchpad/httpd-stack'
CG=json.load(open(SP+'/dashboard/callgraph_lit.json'))
for _k,_v in CG.items(): _v['calls']=sorted(set(_v['calls'])|set(_v.get('lit',[]))|set(_v.get('tails',[])))
A={v['addr'].lower():k for k,v in CG.items()}
def s(addr): return A[addr]
F=lambda k: CG[k]['frame']
D=lambda k: CG[k]['demangled']

# ---- the httpd task prefix, every frame read from `entry` in firmware.dis -----------------
PREFIX_COMMON=['httpd_thread','httpd_sess_enum','httpd_process_session','httpd_sess_process',
               'httpd_req_new','httpd_uri']                       # httpd_parse_req inlined into httpd_req_new
GET_ENTRY =[s('4010ca38')]      # AsyncWebServer::request_handler       (registered for GET + OPTIONS)
POST_ENTRY=[s('4010d688')]      # AsyncWebServer::request_post_handler  (registered for POST)
CHAIN     =[s('4010c284'),      # AuthMiddlewareHandler::handleRequest  (web_server_base auth IS set: main.cpp:1482)
            s('400e2388'),      # WebOriginGuard::handleRequest
            s('401070d4')]      # WebDeviceDashboard::handleRequest
GET_WRAP  =[s('401c1510')]+CHAIN            # request_handler_ dispatches through the vtable
POST_WRAP =[s('4010ca70')]+CHAIN            # handle_raw_body_ dispatches directly (Content-Type: application/json)

# ---- indirect edges, each verified in the disassembly -------------------------------------
EXTRA={
  s('400fad6c'): [],            # json::build_json -> the lambda, filled per route
  s('40104a38'): [],            # run_on_loop_ -> job(): the loop task runs it (see notes)
  # handle_entity_settings_meta_ -> settings->write_settings_meta(): virtual, both overrides
  s('40106a70'): [s('400e4ed0'), s('400e4e2c'), s('401be164')],
  # httpd_send_all -> sd->send_fn: the default, nothing overrides it in web_server_idf
  'httpd_send_all$isra$0': ['httpd_default_send'],
  # /capabilities lambda -> this->rollback_target_() (virtual) and the storage/keeper getters
  s('40106148'): [s('401054ac'), s('400fbb3c'), s('401c062c'), s('401c0624'),
                  s('400df668'), s('400df660')],
  # EthernetComponent::get_eth_mac_address_raw -> esp_eth_ioctl(eth_handle_, ETH_CMD_G_MAC_ADDR, ..)
  s('400f62b8'): [s('4012c99c')],
  # handle_rollback_ -> this->rollback_target_() / this->select_rollback_(), both virtual
  s('40106664'): [s('401054ac'), s('401048bc')],
}
LAMBDA={   # handler -> its json_build_t _M_invoke (l32r'd into the std::function before build_json)
  '40104b4c':'40106054',  # handle_info_
  '40104ba4':'40105a24',  # handle_status_
  '40104bfc':'40105a14',  # handle_network_
  '40104c54':'401064b8',  # handle_capabilities_
  '40104e4c':'40105714',  # handle_auth_get_
  '40104a60':'401057d4',  # send_error_
  '40104af4':'40105790',  # send_success_
}
ABORT={'__assert_func','abort','_esp_error_check_failed','esp_system_abort','panic_abort',
       'esp_log_write','esp_log_writev'}
# job() bodies: dispatched to the loop task through LoopJobSlot, charged to the loop's 8192
LOOPJOB={s(x) for x in ('40106b68','40104744','40104cdc','401046f8','401046a4','401051b0',
                        '401c134c','401046c4','401c11e4','401c120c')}
FLOAT={'_dtoa_r'}   # newlib float printf: no dashboard/IDF format string on these paths has %f/%g

def deepest(root, cut, extra, unroll):
    memo={}
    def go(sym,counts):
        if sym in cut or sym not in CG: return (0,[])
        if counts.get(sym,0)>=unroll.get(sym,1): return (0,[])
        key=(sym,tuple(sorted((k,v) for k,v in counts.items() if k in unroll)))
        if key in memo: return memo[key]
        c2=dict(counts); c2[sym]=counts.get(sym,0)+1
        best=(0,[])
        for c in list(CG[sym]['calls'])+extra.get(sym,[]):
            b,p=go(c,c2)
            if b>best[0]: best=(b,p)
        r=(CG[sym]['frame']+best[0],[(sym,CG[sym]['frame'])]+best[1]); memo[key]=r; return r
    return go(root,{})

UNROLL={ s('400fb604'):11,   # JsonDeserializer<BoundedReader>::parseVariant -- limit 10 (movi.n a12,10 @400fb945)
         s('400ee594'):11,   # JsonSerializer<StaticStringWriter>::accept  -- document nesting (<=4 built here)
         s('4011ca2c'):11,   # JsonSerializer<Writer<std::string>>::accept
         s('400e1168'):11,   # VariantData::clear -- teardown, same depth as the document
         s('400fb038'):11 }  # VariantData::accept<JsonVariantCopier> -- deep copy of the parsed doc

def run(label, entry_addr, post=False, cut_float=True, extra_cut=()):
    pre=PREFIX_COMMON+(POST_ENTRY+POST_WRAP if post else GET_ENTRY+GET_WRAP)
    pren=sum(F(x) for x in pre)
    cut=set(ABORT)|LOOPJOB|set(extra_cut)|({s(x) for x in FLOAT if x in A} | (FLOAT&set(CG)) if cut_float else set())
    extra=dict(EXTRA)
    root=s(entry_addr)
    # splice every build_json->lambda edge that this route's subtree can reach
    # build_json's std::function target is whichever lambda THIS route installed; plus the
    # send_error_/send_success_ lambdas, which are reached through their own build_json call.
    lam=[]
    for h,l in LAMBDA.items():
        if h==entry_addr or h in ('40104a60','40104af4'): lam.append(s(l))
    if entry_addr in ('40104978','40106c30','40105068','401066e4','40106a70','40106f18',
                      '40106608','40106638','40106664','401052a8'):
        lam=[s(LAMBDA['40104a60']),s(LAMBDA['40104af4'])]
    extra[s('400fad6c')]=lam
    b,p=deepest(root,cut,extra,UNROLL)
    # same walk with every ESPHome ESP_LOGx from this task cut: separates the routinely
    # reachable cost from the device-internal error branches that log
    nb,_=deepest(root,cut|{s('400fc244')},extra,UNROLL)
    print('='*100)
    print('%-38s prefix=%4d  subtree=%4d  TOTAL=%4d  free=%5d   | no-log TOTAL=%4d'%(label,pren,b,pren+b,4352-(pren+b),pren+nb))
    for x in pre: print('   %5d  [prefix] %s'%(F(x),D(x)[:120]))
    for sym,f in p:  print('   %5d  %s'%(f,D(sym)[:120]))
    return pren, b, [(D(x),F(x)) for x in pre]+[(D(sym),f) for sym,f in p]

if __name__=='__main__':
    ROUTES=[('GET /              (handle_page_)','40104978',False),
            ('GET /api/device/info','40104b4c',False),
            ('GET /api/device/status','40104ba4',False),
            ('GET /api/device/network','40104bfc',False),
            ('GET /api/device/capabilities','40104c54',False),
            ('GET /api/device/auth','40104e4c',False),
            ('POST /api/device/auth','40106f18',True),
            ('POST /api/device/system/reboot','40106608',True),
            ('POST /api/device/system/factory-reset','40106638',True),
            ('POST /api/device/system/rollback','40106664',True),
            ('GET /api/device/entities','40106c30',False),
            ('GET /api/device/entity-settings','40105068',False),
            ('POST /api/device/entity-settings','401066e4',True),
            ('GET /api/device/entity-settings-meta','40106a70',False),
            ('(any 4xx) send_error_','40104a60',False),
            ('(prefix only) canHandle url_','401052a8',False),
            ('GET /api/device/auth  [503 branch cut]','40104e4c',False),
            ]
    out={}
    for lab,ad,post in ROUTES:
        pren,b,chain=run(lab,ad,post,extra_cut=([s('40104a60')] if '503 branch cut' in lab else ()))
        out[lab]=(pren,b,pren+b,chain)
    json.dump({k:{'prefix':v[0],'subtree':v[1],'total':v[2],'chain':v[3]} for k,v in out.items()},
              open(SP+'/dashboard/routes_lit.json','w'),indent=1)
