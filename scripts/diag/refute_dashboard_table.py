import re
exec(open("walk.py").read().split('if __name__')[0])
# final proven/argued cuts
EDGE_CUTS.update({
 ("__ssprint_r","_free_r"),("__ssprint_r","memcpy"),
 ("_svfprintf_r","_dtoa_r"),("_svfprintf_r","frexp"),
})
# drop __assert_func entirely (fatal abort leaf) by making it zero-cost
ASSERT=[a for a,f in F.items() if f['name']=='__assert_func']
for a in ASSERT: F[a]['frame']=0
ser=[a for a,f in F.items() if f['dn']=="esphome::json::JsonBuilder::serialize()"][0]
logf=[a for a,f in F.items() if f['dn'].startswith("esphome::esp_log_printf_(int")][0]
OOM={(F[ser]['name'],F[logf]['name'])}
ag=[a for a,f in F.items() if re.search(r'handle_auth_get_\(',f['dn']) and '_M_' not in f['dn'] and 'lambda' not in f['dn']][0]
se=[a for a,f in F.items() if re.search(r'WebDeviceDashboard::send_error_\(',f['dn'])][0]
DEAD503={(F[ag]['name'],F[se]['name'])}
PG,PP=688,816
routes=[("GET /api/device/auth","handle_auth_get_",PG),("POST /api/device/auth","handle_auth_set_",PP),
("POST /api/device/system/reboot","handle_reboot_",PP),("POST /api/device/system/factory-reset","handle_factory_reset_",PP),
("POST /api/device/system/rollback","handle_rollback_",PP),("POST /api/device/entity-settings","handle_entity_settings_set_",PP),
("GET /api/device/entity-settings","handle_entity_settings_get_",PG),("GET /api/device/entity-settings-meta","handle_entity_settings_meta_",PG),
("GET /api/device/info","handle_info_",PG),("GET /api/device/status","handle_status_",PG),
("GET /api/device/network","handle_network_",PG),("GET /api/device/capabilities","handle_capabilities_",PG),
("GET /api/device/entities","handle_entities_",PG),("GET /","handle_page_",PG)]
def run(a,cuts):
    save=set(EDGE_CUTS); EDGE_CUTS.update(cuts)
    t,p,r=deepest(a,True,()); EDGE_CUTS.clear(); EDGE_CUTS.update(save); return t,p,r
print(f"{'route':40s} {'W_static':>9s} {'W_client':>9s}  chain tail")
out={}
for lab,n,pre in routes:
    a=[x for x,f in F.items() if re.search(r'WebDeviceDashboard::'+n+r'\(',f['dn']) and 'lambda' not in f['dn'] and '_M_' not in f['dn']][0]
    extra = DEAD503 if n=="handle_auth_get_" else set()
    ts,ps,_=run(a,extra); tc,pc,rc=run(a,extra|OOM)
    out[lab]=(ts+pre,tc+pre,pc,rc,pre)
    print(f"{lab:40s} {ts+pre:9d} {tc+pre:9d}  {' > '.join(F[x]['dn'].split('(')[0].split('::')[-1][:22] for x in pc[:6])}")
print()
for lab,(s,c,pc,rc,pre) in sorted(out.items(), key=lambda kv:-kv[1][1])[:6]:
    print(f"--- {lab}: W_client={c} (prefix {pre}) rec_cut={rc}")
    for x in pc: print(f"      {F[x]['frame']:5d}  {F[x]['dn'][:100]}")
