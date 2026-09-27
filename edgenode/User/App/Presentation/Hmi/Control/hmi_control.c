#include "hmi_control.h"
#include "Presentation/Hmi/Page/hmi_page.h"
#include "Presentation/Hmi/Render/hmi_render.h"
#include "Presentation/Hmi/hmi_types.h"

static uint8_t refresh_pending;
static uint16_t refresh_first_y;
static uint16_t refresh_last_y;

static hmi_page_id_t current_page;   /* 当前显示页 */
static hmi_page_id_t selected_page;  /* 候选页 */
static uint8_t initialized;          /* 是否已经初始化 */

static hmi_view_data_t view_data;

/* 合并待刷新的行范围，避免连续请求互相覆盖。 */
static void request_refresh(uint16_t first_y, uint16_t last_y)
{
    if(refresh_pending == 0U)
    {
        refresh_first_y = first_y;
        refresh_last_y  = last_y;
        refresh_pending = 1U;
        return;
    }

    if(first_y < refresh_first_y)
    {
        refresh_first_y = first_y;
    }
    if(last_y > refresh_last_y)
    {
        refresh_last_y = last_y;
    }
}

void hmi_control_init(uint8_t node_id)
{
    if(initialized != 0U)
    {
        return;
    }

    current_page = HMI_PAGE_HOME;
    selected_page = HMI_PAGE_HOME;
    view_data.node_id = node_id;

    initialized = 1U;
    request_refresh(0U, (uint16_t)(HMI_HEIGHT - 1U));
}

void hmi_key_event(hmi_key_t key)
{
    if(initialized != 1U)
    {
        return;
    }

    switch(key)
    {
        case HMI_KEY_PREVIOUS: // 按下上一页
            if(selected_page > HMI_PAGE_HOME)
            {
                selected_page = (hmi_page_id_t)(selected_page - 1U);
                request_refresh(212U, (uint16_t)(HMI_HEIGHT - 1U));
            }
        break;

        case HMI_KEY_NEXT: // 按下下一页
            if(selected_page < (HMI_PAGE_COUNT - 1U))
            {
                selected_page = (hmi_page_id_t)(selected_page + 1U);
                request_refresh(212U, (uint16_t)(HMI_HEIGHT - 1U));
            }
        break;

        case HMI_KEY_CONFIRM:
            if(current_page != selected_page)
            {
                current_page = selected_page;
                request_refresh(0U, 211U);
            }
        break;

        default:
        break;
    }
}

/*
    这个函数就是负责整个HMI层的联动
*/
uint8_t hmi_service(void)
{
    hmi_page_t page;
    hmi_render_result_enum render_result;

    //可靠检查
    if(initialized == 0U)
    {
        return 0U;
    }

    /* 推进 Render 中已经开始的绘制任务。 */
    hmi_render_service();

    /* 没有新刷新请求，或 Render 还忙，就先不提交新页面。 */
    if((refresh_pending == 0U) || (hmi_render_busy() != 0U))
    {
        return 1U;
    }

    /* 用 Control 保存的数据和页码，生成 Page 描述。 */
    if(hmi_page_build(&page, &view_data, current_page, selected_page) == 0U)
    {
        return 0U;
    }

    /* 把指定行范围交给 Render，尝试创建新的绘制任务。 */
    render_result = hmi_render_page(&page,
                                    refresh_first_y,
                                    refresh_last_y);

    if(render_result == HMI_RENDER_ACCEPTED)
    {
        refresh_pending = 0U;
    }
    else if(render_result == HMI_RENDER_ERROR)
    {
        return 0U;
    }

    /* BUSY 时保留 refresh_pending，下次 service 再试。 */
    return 1U;
}
