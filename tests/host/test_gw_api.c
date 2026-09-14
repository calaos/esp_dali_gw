#include <string.h>
#include "unity.h"
#include "gw_api.h"

/* The error set is a wire contract: a renamed string silently breaks every client. */
static void test_err_str_covers_the_closed_set(void)
{
    TEST_ASSERT_EQUAL_STRING("ok", gw_api_err_str(GW_OK));
    TEST_ASSERT_EQUAL_STRING("invalid_arg", gw_api_err_str(GW_ERR_INVALID_ARG));
    TEST_ASSERT_EQUAL_STRING("bus_busy", gw_api_err_str(GW_ERR_BUS_BUSY));
    TEST_ASSERT_EQUAL_STRING("bus_unpowered", gw_api_err_str(GW_ERR_BUS_UNPOWERED));
    TEST_ASSERT_EQUAL_STRING("no_reply", gw_api_err_str(GW_ERR_NO_REPLY));
    TEST_ASSERT_EQUAL_STRING("tx_failed", gw_api_err_str(GW_ERR_TX_FAILED));
    TEST_ASSERT_EQUAL_STRING("timeout", gw_api_err_str(GW_ERR_TIMEOUT));
    TEST_ASSERT_EQUAL_STRING("not_present", gw_api_err_str(GW_ERR_NOT_PRESENT));
    TEST_ASSERT_EQUAL_STRING("address_in_use", gw_api_err_str(GW_ERR_ADDRESS_IN_USE));
    TEST_ASSERT_EQUAL_STRING("unsupported", gw_api_err_str(GW_ERR_UNSUPPORTED));
    TEST_ASSERT_EQUAL_STRING("cancelled", gw_api_err_str(GW_ERR_CANCELLED));
    TEST_ASSERT_EQUAL_STRING("internal", gw_api_err_str(GW_ERR_INTERNAL));
}

static void test_err_from_esp_never_invents_a_code(void)
{
    TEST_ASSERT_EQUAL(GW_OK, gw_api_err_from_esp(ESP_OK));
    TEST_ASSERT_EQUAL(GW_ERR_INVALID_ARG, gw_api_err_from_esp(ESP_ERR_INVALID_ARG));
    TEST_ASSERT_EQUAL(GW_ERR_TIMEOUT, gw_api_err_from_esp(ESP_ERR_TIMEOUT));
    /* An unmapped driver code must still land inside the closed set. */
    TEST_ASSERT_EQUAL(GW_ERR_INTERNAL, gw_api_err_from_esp(0x7fff));
}

static void test_result_free_is_safe_on_a_zeroed_struct(void)
{
    gw_result_t res;
    memset(&res, 0, sizeof(res));
    gw_api_result_free(&res);
    TEST_ASSERT_NULL(res.data);
}

/* /api/info and the MQTT status topic must expose the same status fields (SPEC 8.1, 9). */
static void test_status_and_info_agree(void)
{
    gw_info_t info;
    memset(&info, 0, sizeof(info));
    strcpy(info.state, "online");
    strcpy(info.fw, "1.2.3");
    strcpy(info.idf, "6.0.2");
    strcpy(info.ip, "192.168.1.42");
    strcpy(info.mac, "aa:bb:cc:dd:ee:ff");
    strcpy(info.mode, "sta");
    info.rssi = -61;
    info.uptime_s = 1234;

    cJSON *status = gw_api_status_to_json(&info);
    cJSON *full = gw_api_info_to_json(&info);
    TEST_ASSERT_NOT_NULL(status);
    TEST_ASSERT_NOT_NULL(full);

    const char *shared[] = {"state", "fw", "idf", "ip", "rssi", "uptime_s", "mac"};
    for (size_t i = 0; i < sizeof(shared) / sizeof(shared[0]); i++) {
        cJSON *a = cJSON_GetObjectItem(status, shared[i]);
        cJSON *b = cJSON_GetObjectItem(full, shared[i]);
        TEST_ASSERT_NOT_NULL_MESSAGE(a, shared[i]);
        TEST_ASSERT_NOT_NULL_MESSAGE(b, shared[i]);
        TEST_ASSERT_TRUE_MESSAGE(cJSON_Compare(a, b, true), shared[i]);
    }

    /* The status topic carries nothing beyond those seven fields. */
    TEST_ASSERT_EQUAL(7, cJSON_GetArraySize(status));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(full, "mode"));

    cJSON_Delete(status);
    cJSON_Delete(full);
}

void test_gw_api_run(void)
{
    RUN_TEST(test_err_str_covers_the_closed_set);
    RUN_TEST(test_err_from_esp_never_invents_a_code);
    RUN_TEST(test_result_free_is_safe_on_a_zeroed_struct);
    RUN_TEST(test_status_and_info_agree);
}
