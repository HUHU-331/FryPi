/* Private includes -----------------------------------------------------------*/
//includes
#include "user_TasksInit.h"
#include "user_ScrRenewTask.h"


/* Private typedef -----------------------------------------------------------*/

/* Private define ------------------------------------------------------------*/

/* Private variables ---------------------------------------------------------*/

/* Private function prototypes -----------------------------------------------*/


/**
  * @brief  Page request forwarder
  * @param  argument: Not used
  * @retval None
  * @note   This task used to call Page_Back()/Page_Back_Bottom() directly from
  *         its own context, i.e. it created and deleted LVGL objects while
  *         LvHandlerTask was walking the same object tree for rendering.
  *         LVGL has no internal locking, so that was a real data race: the
  *         renderer could dereference a node that had just been freed
  *         (sporadic HardFault, timing dependent).
  *         Now this task only translates "key event" into "page request" and
  *         pushes it to PageReq_MessageQueue; the lv_* calls are executed by
  *         LvHandlerTask alone, so read and modify can never overlap.
  *         Keep this file free of any lv_* call.
  */
void ScrRenewTask(void *argument)
{
	uint8_t keystr=0;
	while(1)
	{
		if(osMessageQueueGet(Key_MessageQueue,&keystr,NULL,0)==osOK)
		{
			/* forward: key event (1 short / 2 long) -> page request */
			osMessageQueuePut(PageReq_MessageQueue,&keystr,0,0);
		}
		osDelay(10);
	}
}
