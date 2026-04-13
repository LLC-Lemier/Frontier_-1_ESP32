static esp_err_t network_static_post_handler(httpd_req_t *req)
{
    char buffer[512];
    int total = req->content_len;

    if (total <= 0 || total >= (int)sizeof(buffer)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large or empty");
        return ESP_ERR_INVALID_SIZE;
    }

    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, buffer + received, total - received);
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read request body");
            return ESP_FAIL;
        }
        received += ret;
    }
    buffer[received] = '\0';

    cJSON *root = cJSON_Parse(buffer);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    network_config_t cfg = {0};
    cfg.dhcp_enabled = false;

    bool ok =
        parse_ipv4_string(root, "ip", &cfg.ip, true) &&
        parse_ipv4_string(root, "netmask", &cfg.netmask, true) &&
        parse_ipv4_string(root, "gateway", &cfg.gateway, true) &&
        parse_ipv4_string(root, "dns1", &cfg.dns1, false) &&
        parse_ipv4_string(root, "dns2", &cfg.dns2, false);

    cJSON_Delete(root);

    if (!ok) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid IPv4 fields");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = network_config_save(&cfg);
    if (err == ESP_OK) {
        err = network_config_apply(&cfg);
    }

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to apply network config");
        return err;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"Static IP applied\"}");
    return ESP_OK;
}