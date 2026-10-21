/* Private includes -----------------------------------------------------------*/
//includes
#include "user_TasksInit.h"
#include "ui_HomePage.h"
#include "main.h"
#include "key.h"
#include "FreeRTOS.h"
#include "task.h"

/* Private typedef -----------------------------------------------------------*/

/* Private define ------------------------------------------------------------*/

/* Private variables ---------------------------------------------------------*/

/* Private function prototypes -----------------------------------------------*/


/**
  * @brief  Key press check task
  * @param  argument: Not used
  * @retval None
  * @note   Old version polled KeyScan() every 1 ms and KeyScan() itself blocked
  *         3~6 ms with osDelay() for debouncing. Now the task sleeps until the
  *         EXTI ISR notifies it, and the 50 ms timeout only serves long-press
  *         detection and as a fallback if an edge is missed.
  */
void KeyTask(void *argument)
{
	uint8_t keystr=0;
	uint8_t Stopstr=0;
	uint8_t IdleBreakstr=0;
	while(1)
	{
		ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));

		switch(KeyScan(0))
		{
			case 1:
				keystr = 1;
				osMessageQueuePut(Key_MessageQueue, &keystr, 0, 1);
				osMessageQueuePut(IdleBreak_MessageQueue, &IdleBreakstr, 0, 1);
				break;

			case 2:
				if(Page_Get_NowPage()->page_obj == &ui_HomePage)
				{
					osMessageQueuePut(Stop_MessageQueue, &Stopstr, 0, 1);
				}
				else
				{
					keystr = 2;
					osMessageQueuePut(Key_MessageQueue, &keystr, 0, 1);
					osMessageQueuePut(IdleBreak_MessageQueue, &IdleBreakstr, 0, 1);
				}
				break;
		}
	}
}
