#include "work_queue.h"
#include "global_state.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "mining.h"
#include <limits.h>
#include "string.h"

#include <sys/time.h>

static const char *TAG = "create_jobs_task";

static char *miner_extranonce1 = NULL;
static int64_t miner_extranonce1_time = 0;

#define QUEUE_LOW_WATER_MARK 10 // Adjust based on your requirements

static bool should_generate_more_work(GlobalState *GLOBAL_STATE);
static void generate_work(GlobalState *GLOBAL_STATE, mining_notify *notification, const char *extranonce_1 ,uint32_t extranonce_2);

static char *generate_miner_extranonce1(void)
{
    uint32_t random_value;
    char *extranonce1;

    random_value = esp_random();

    extranonce1 = malloc(9);
    if (extranonce1 == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate miner extranonce1");
        return NULL;
    }

    snprintf(extranonce1, 9, "%08lx", (unsigned long)random_value);

    ESP_LOGI(TAG, "New miner extranonce1: %s", extranonce1);

    return extranonce1;
}

void create_jobs_task(void *pvParameters)
{
    GlobalState *GLOBAL_STATE = (GlobalState *)pvParameters;
	miner_extranonce1 = generate_miner_extranonce1();
	if (miner_extranonce1 == NULL)
	{
		ESP_LOGE(TAG, "Failed to generate initial miner extranonce1");
		vTaskDelete(NULL);
		return;
	}

	miner_extranonce1_time = esp_timer_get_time();

    while (1)
    {
        mining_notify *mining_notification = (mining_notify *)queue_dequeue(&GLOBAL_STATE->stratum_queue);
        if (mining_notification == NULL) {
            ESP_LOGE(TAG, "Failed to dequeue mining notification");
            vTaskDelay(100 / portTICK_PERIOD_MS); // Wait a bit before trying again
            continue;
        }

        ESP_LOGI(TAG, "New Work Dequeued %s", mining_notification->job_id);

        if (GLOBAL_STATE->new_stratum_version_rolling_msg) {
            ESP_LOGI(TAG, "Set chip version rolls %i", (int)(GLOBAL_STATE->version_mask >> 13));
            (GLOBAL_STATE->ASIC_functions.set_version_mask)(GLOBAL_STATE->version_mask);
            GLOBAL_STATE->new_stratum_version_rolling_msg = false;
        }

        uint32_t extranonce_2 = 0;
        while (GLOBAL_STATE->stratum_queue.count < 1 && GLOBAL_STATE->abandon_work == 0)
        {
			int64_t now = esp_timer_get_time();
			if ((now - miner_extranonce1_time) >= 1000000)
			{
				char *new_extranonce1 = generate_miner_extranonce1();
				if (new_extranonce1 != NULL)
				{
					free(miner_extranonce1);
					miner_extranonce1 = new_extranonce1;
					miner_extranonce1_time = now;
					extranonce_2 = 0;
					ESP_LOGI(TAG, "E1 changed, E2 reset to 0");
				}
			}
            if (should_generate_more_work(GLOBAL_STATE))
            {
                generate_work(GLOBAL_STATE, mining_notification, miner_extranonce1, extranonce_2);
                // Increase extranonce_2 for the next job.
                extranonce_2++;
            }
            else
            {
                // If no more work needed, wait a bit before checking again.
                vTaskDelay(100 / portTICK_PERIOD_MS);
            }
        }

        if (GLOBAL_STATE->abandon_work == 1)
        {
            GLOBAL_STATE->abandon_work = 0;
            ASIC_jobs_queue_clear(&GLOBAL_STATE->ASIC_jobs_queue);
            xSemaphoreGive(GLOBAL_STATE->ASIC_TASK_MODULE.semaphore);
        }

        STRATUM_V1_free_mining_notify(mining_notification);
    }
}

static bool should_generate_more_work(GlobalState *GLOBAL_STATE)
{
    return GLOBAL_STATE->ASIC_jobs_queue.count < QUEUE_LOW_WATER_MARK;
}

static void generate_work(GlobalState *GLOBAL_STATE, mining_notify *notification, const char *extranonce_1, uint32_t extranonce_2)
{
    char *extranonce_2_str = extranonce_2_generate(extranonce_2, GLOBAL_STATE->extranonce_2_len);
    if (extranonce_2_str == NULL) {
        ESP_LOGE(TAG, "Failed to generate extranonce_2");
        return;
    }

    char *coinbase_tx = construct_coinbase_tx(notification->coinbase_1, notification->coinbase_2, extranonce_1, extranonce_2_str);
    if (coinbase_tx == NULL) {
        ESP_LOGE(TAG, "Failed to construct coinbase_tx");
        free(extranonce_2_str);
        return;
    }

    char *merkle_root = calculate_merkle_root_hash(coinbase_tx, (uint8_t(*)[32])notification->merkle_branches, notification->n_merkle_branches);
    if (merkle_root == NULL) {
        ESP_LOGE(TAG, "Failed to calculate merkle_root");
        free(extranonce_2_str);
        free(coinbase_tx);
        return;
    }

    bm_job next_job = construct_bm_job(notification, merkle_root, GLOBAL_STATE->version_mask);

    bm_job *queued_next_job = malloc(sizeof(bm_job));
    if (queued_next_job == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for queued_next_job");
        free(extranonce_2_str);
        free(coinbase_tx);
        free(merkle_root);
        return;
    }

    memcpy(queued_next_job, &next_job, sizeof(bm_job));
    queued_next_job->extranonce2 = extranonce_2_str; // Transfer ownership
	queued_next_job->extranonce1 = strdup(extranonce_1);
    queued_next_job->jobid = strdup(notification->job_id);
    queued_next_job->version_mask = GLOBAL_STATE->version_mask;

    queue_enqueue(&GLOBAL_STATE->ASIC_jobs_queue, queued_next_job);

    free(coinbase_tx);
    free(merkle_root);
}