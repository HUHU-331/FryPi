#include "key.h"
#include "delay.h"
#include "user_TasksInit.h"
#include "FreeRTOS.h"
#include "task.h"

/* Debounce window: a new falling edge inside this time is treated as bounce */
#define KEY_DEBOUNCE_MS     20U
/* Hold time to report a long press */
#define KEY_LONGPRESS_MS  1000U

/* Defined in user_TasksInit.c, used to wake the key task from the EXTI ISR */
extern osThreadId_t KeyTaskHandle;

void Key_Port_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOA_CLK_ENABLE();

  /*Configure GPIO pin : PA0 */
  GPIO_InitStruct.Pin = KEY1_PIN;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(KEY1_PORT, &GPIO_InitStruct);

  /* EXTI interrupt init
     NOTE: priority must be >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY (5),
     otherwise vTaskNotifyGiveFromISR() trips the FreeRTOS assertion.
     The original code used 0, which is illegal once the ISR calls RTOS APIs. */
  HAL_NVIC_SetPriority(EXTI0_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(EXTI0_IRQn);
}

/* EXTI callback: keep it short, only wake the key task.
   No debounce, no business logic, no blocking call in here. */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;

  if (GPIO_Pin == KEY1_PIN)
  {
    vTaskNotifyGiveFromISR((TaskHandle_t)KeyTaskHandle, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
  }
}

/* Non-blocking key scan, called from task context.
   return 1 = short press (fired on release)
   return 2 = long press  (fired once while still held)
   return 0 = no event
   The old version called osDelay(3) twice, blocking the task 3~6 ms per call. */
uint8_t KeyScan(uint8_t mode)
{
	static uint32_t accept_tick = 0;   /* last accepted edge, for debounce */
	static uint32_t press_tick  = 0;   /* time when the key went down */
	static uint8_t  key_down    = 0;
	static uint8_t  long_sent   = 0;

	uint32_t now = HAL_GetTick();
	uint8_t  keyvalue = 0;

	if (mode)
	{
		key_down  = 0;
		long_sent = 0;
	}

	if (KEY1 == GPIO_PIN_RESET)
	{
		if (key_down == 0)
		{
			if ((now - accept_tick) < KEY_DEBOUNCE_MS)
			{
				return 0;                 /* still bouncing, ignore */
			}
			accept_tick = now;
			press_tick  = now;
			key_down    = 1;
		}
		else if ((long_sent == 0) && ((now - press_tick) >= KEY_LONGPRESS_MS))
		{
			long_sent = 1;
			keyvalue  = 2;                /* long press fires while held */
		}
	}
	else
	{
		if (key_down != 0)
		{
			key_down = 0;
			/* a contact shorter than the debounce window is bounce, not a press */
			if ((long_sent == 0) && ((now - press_tick) >= KEY_DEBOUNCE_MS))
			{
				keyvalue = 1;             /* short press fires on release */
			}
		}
	}

	return keyvalue;
}
