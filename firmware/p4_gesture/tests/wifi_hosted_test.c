/* Included after the complete production bsp_wifi_hosted.c by the host runner.
 * IDF/RTOS doubles are deterministic; no hardware is accessed. */
static void reset_test(void)
{
    memset(&s_wifi,0,sizeof(s_wifi));memset(&s_req,0,sizeof(s_req));
    memset(s_saved_ssid,0,sizeof(s_saved_ssid));memset(s_saved_pass,0,sizeof(s_saved_pass));
    memset(s_target_pass,0,sizeof(s_target_pass));
    s_radio_ready=s_started=s_save_pending=s_link_up=s_scan_ready=false;
    s_disconnect_wait=s_connect_pending=s_events=false;s_netif=NULL;
    s_scan_deadline=s_join_deadline=s_scan_cooldown=s_rejoin_at_ms=0;
    s_scan_status=0;s_rejoin_left=0;s_stage="IDLE";
    fake_time=1000000;creates=scans=stops=clears=record_gets=writes=commits=connects=0;
    config_calls=0;scan_error=records_error=commit_error=0;disconnect_error=ESP_ERR_WIFI_NOT_CONNECT;
    scan_count=0;stored_valid=false;memset(records,0,sizeof(records));
}
static void radio(void) {s_radio_ready=true;s_wifi.state=BSP_WIFI_READY;}
static void scan_done(unsigned status)
{wifi_event_sta_scan_done_t e={.status=status};on_wifi_event(NULL,0,WIFI_EVENT_SCAN_DONE,&e);}
static void disconnected(void)
{wifi_event_sta_disconnected_t d={.reason=202};on_wifi_event(NULL,0,WIFI_EVENT_STA_DISCONNECTED,&d);}
static void got_ip(void)
{ip_event_got_ip_t ip={0};on_ip_event(NULL,0,IP_EVENT_STA_GOT_IP,&ip);}
static void connected(void)
{
    wifi_event_sta_connected_t d={0};d.ssid_len=(uint8_t)strlen(s_wifi.ssid);
    memcpy(d.ssid,s_wifi.ssid,d.ssid_len);on_wifi_event(NULL,0,WIFI_EVENT_STA_CONNECTED,&d);
}
static void ap(unsigned i,const char *name,int rssi)
{snprintf((char*)records[i].ssid,sizeof(records[i].ssid),"%s",name);records[i].rssi=rssi;}
static void begin_join(const char *name,const char *pass)
{
    assert(bsp_wifi_set_credentials(name,pass));service_step();service_step();
    assert(s_wifi.state==BSP_WIFI_JOINING);
}
int main(void)
{
    reset_test();bsp_wifi_hosted_init();assert(creates==2);
    bsp_wifi_hosted_init();assert(creates==2);
    assert(bsp_wifi_scan());assert(!bsp_wifi_scan());service_step();assert(scans==0);
    bringup_worker(NULL);assert(!reset_enabled && attach_calls==1);
    service_step();assert(scans==1 && s_wifi.scanning);
    assert(!bsp_wifi_set_credentials("AP","12345678"));
    scan_done(0);assert(s_wifi.scanning && s_wifi.scan_done_seq==0 && record_gets==0);
    scan_count=7;
    ap(0,"中文热点",-40);ap(1,"中文热点",-48);ap(2,"Test Net,=x",-65);
    ap(3,"12345678901234567890123456789012",-110);ap(4,"",-40);
    ap(5,"bad\nssid",-40);ap(6,"weak",-125);
    service_step();assert(!s_wifi.scanning && s_wifi.scan_done_seq==1);
    assert(s_wifi.scan_raw==7 && s_wifi.ap_count==4 && clears==1);
    assert(!strcmp(s_wifi.ap_ssid[0],"中文热点"));
    assert(!strcmp(s_wifi.ap_ssid[1],"Test Net,=x"));
    assert(strlen(s_wifi.ap_ssid[2])==32 && s_wifi.ap_rssi[2]==-110);
    assert(s_wifi.ap_rssi[3]==-125);
    char names[6][33]; int signals[6]; bsp_wifi_status_t snapshot;
    assert(bsp_wifi_snapshot(names,signals,6,&snapshot)==4);
    assert(snapshot.scan_done_seq==1 && snapshot.ap_count==4 && !snapshot.scanning);
    assert(!strcmp(names[0],"中文热点") && signals[3]==-125);
    assert(bsp_wifi_hosted_command("WIFI APS abcd"));
    assert(strstr(usb_line,"encoding=hex") && !strstr(usb_line,"中文热点"));
    assert(bsp_wifi_hosted_command("WIFI AP abcd 0"));
    assert(strstr(usb_line,"ssid_hex=") && !strstr(usb_line,"中文热点"));
    assert(strlen(usb_line)<sizeof(usb_line));
    strcpy(s_wifi.ssid,"Net state=ERROR");
    assert(bsp_wifi_hosted_command("WIFI STATUS abcd"));
    assert(strstr(usb_line,"state=READY") && !strstr(usb_line,"state=ERROR"));
    assert(strstr(usb_line,"ssid_hex=4e65742073746174653d4552524f52"));
    puts("PASS scan startup queue, atomic busy, commit-after-event, UTF8/space/full SSID, weak APs, dedup and framed USB");

    records_error=ESP_FAIL;assert(bsp_wifi_scan());service_step();scan_done(0);service_step();
    assert(s_wifi.scan_error==ESP_FAIL && s_wifi.ap_count==4 && s_wifi.scan_done_seq==2);
    records_error=0;scan_count=0;assert(bsp_wifi_scan());service_step();scan_done(0);service_step();
    assert(s_wifi.ap_count==0 && s_wifi.scan_error==0);
    assert(bsp_wifi_scan());service_step();scan_done(1);service_step();assert(s_wifi.scan_error);
    scan_error=ESP_FAIL;assert(bsp_wifi_scan());service_step();assert(!s_wifi.scanning && s_wifi.scan_error);
    scan_error=0;assert(bsp_wifi_scan());service_step();fake_time+=21000000;service_step();
    assert(stops==1 && !s_wifi.scanning && s_wifi.scan_error==ESP_ERR_TIMEOUT);
    assert(!bsp_wifi_scan());scan_done(0);assert(!s_scan_ready);fake_time+=1001000;
    s_wifi.state=BSP_WIFI_ERROR;assert(bsp_wifi_scan());assert(creates==2);service_step();
    scan_done(0);service_step();assert(!s_wifi.scan_error);
    s_wifi.state=BSP_WIFI_ONLINE;strcpy(s_wifi.ip,"1.2.3.4");assert(bsp_wifi_scan());service_step();
    scan_done(0);service_step();assert(s_wifi.state==BSP_WIFI_ONLINE && s_wifi.ip[0]);
    puts("PASS RPC failure != empty scan, clear driver storage, event failure/start failure, deadline/late event, no retry/reset worker, online scan");

    reset_test();radio();
    assert(!bsp_wifi_set_credentials("AP","short"));
    char ssid[34]="123456789012345678901234567890123",hex[65];
    memset(hex,'a',64);hex[64]=0;assert(!bsp_wifi_set_credentials(ssid,hex));ssid[32]=0;
    begin_join(ssid,hex);assert(config_calls==1 && connects==1);
    assert(!memcmp(config.sta.ssid,ssid,32) && !memcmp(config.sta.password,hex,64));
    assert(writes==0);assert(!bsp_wifi_scan());
    got_ip();assert(s_wifi.state==BSP_WIFI_JOINING && !s_save_pending);
    connected();got_ip();assert(s_wifi.state==BSP_WIFI_ONLINE && s_save_pending && writes==0);
    service_step();assert(writes==1 && commits==1 && !strcmp(s_saved_ssid,ssid));
    assert(!memcmp(s_saved_pass,hex,65));
    assert(bsp_wifi_join_saved(ssid));assert(!bsp_wifi_set_credentials("other","12345678"));
    service_step();service_step();assert(!strcmp(s_wifi.ssid,ssid));
    puts("PASS strict byte limits/PSK validation, exact 32/64-byte config, no inline persistence, stale IP ignored, save only after DHCP, request cannot overwrite");

    reset_test();radio();strcpy(s_saved_ssid,"Known Good");strcpy(s_saved_pass,"original8");
    begin_join("Bad Target","12345678");disconnected();assert(writes==0);
    for(int n=0;n<3;++n){fake_time+=5001000;service_step();service_step();disconnected();}
    assert(connects==4 && s_rejoin_left==0);fake_time+=60000000;service_step();assert(connects==4);
    assert(!strcmp(s_saved_ssid,"Known Good"));
    assert(bsp_wifi_scan());service_step();scan_done(0);service_step();assert(creates==0);
    puts("PASS exactly three retries, exhausted budget not refilled, failed credentials preserve saved network, radio scan after auth failure without transport reset");

    reset_test();radio();disconnect_error=ESP_OK;s_link_up=true;strcpy(s_wifi.ip,"1.2.3.4");
    assert(bsp_wifi_set_credentials("Switch AP","12345678"));service_step();service_step();
    assert(config_calls==0 && s_disconnect_wait);disconnected();service_step();assert(config_calls==1);
    connected();got_ip();commit_error=ESP_FAIL;service_step();
    assert(s_wifi.state==BSP_WIFI_ONLINE && s_wifi.save_error && !s_saved_ssid[0]);
    on_ip_event(NULL,0,IP_EVENT_STA_LOST_IP,NULL);
    assert(!s_wifi.ip[0] && s_wifi.state==BSP_WIFI_ERROR && s_rejoin_left==3);
    reset_test();radio();begin_join("Silent DHCP","12345678");fake_time+=31000000;service_step();
    assert(s_wifi.state==BSP_WIFI_ERROR && s_wifi.error==ESP_ERR_TIMEOUT);
    puts("PASS AP switch waits for disconnect, failed commit surfaced, lost-IP invalidates online, bounded DHCP timeout");
    return 0;
}
