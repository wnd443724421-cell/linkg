# 针对GitHub main当前“Scheduler先选Target，Transport再转发”的forward实现。
# 不使用文件库里的旧Transport->Scheduler版本覆盖main。

# 在transport_forward.c中为当前context增加本文件私有校验（不要调用transport_tx.c里的static helper）：

/**
 * @brief 校验Scheduler已经确定的中继发送上下文。
 */
static int _linkg_transport_forward_validate_context(const linkg_transport_tx_context_t *context)
{
    uint32_t index;

    if (context == NULL || context->targets == NULL)
    {
        return -EINVAL;
    }

    if (context->target_count == 0U || context->target_count > LINKG_TRANSPORT_TX_TARGET_MAX)
    {
        return -EINVAL;
    }

    if (!linkg_transport_class_valid(context->traffic_class))
    {
        return -EINVAL;
    }

    for (index = 0U; index < context->target_count; index++)
    {
        if (context->targets[index].link == NULL || context->targets[index].path == NULL)
        {
            return -EINVAL;
        }
    }

    return 0;
}

# 在当前linkg_transport_forward_batch(...)入口，任何数组写入/packet访问之前：

    if (items == NULL || results == NULL || count == 0U)
    {
        return -EINVAL;
    }

    if (count > LINKG_TRANSPORT_FORWARD_BATCH_MAX)
    {
        return -EOVERFLOW;
    }

    ret = _linkg_transport_forward_validate_context(context);
    if (ret != 0)
    {
        return ret;
    }

    for (index = 0U; index < count; index++)
    {
        if (items[index].packet == NULL)
        {
            return -EINVAL;
        }
    }

# 如果你当前forward_batch没有results参数，只删除上面的results == NULL检查；
# context/count/packet三个边界必须保留。
