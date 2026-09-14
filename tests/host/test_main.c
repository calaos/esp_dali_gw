#include "unity.h"

void test_gw_api_run(void);
void test_gw_api_config_run(void);
void test_app_config_run(void);

void setUp(void)
{
}
void tearDown(void)
{
}

int main(void)
{
    UNITY_BEGIN();
    test_gw_api_run();
    test_gw_api_config_run();
    test_app_config_run();
    return UNITY_END();
}
