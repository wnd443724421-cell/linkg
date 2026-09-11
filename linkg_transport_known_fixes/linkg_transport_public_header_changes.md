# include/linkg/core/transport/linkg_transport.h
# 删除已经过时的“Transport Wire Frame单批最大32”公共宏：

- #define LINKG_TRANSPORT_FRAME_BATCH_MAX 32U

# TX逻辑32/Wire64现在属于transport_tx.c私有实现常量：
# LINKG_TRANSPORT_TX_BATCH_MAX       32U
# LINKG_TRANSPORT_TX_WIRE_BATCH_MAX (LINKG_TRANSPORT_TX_BATCH_MAX * LINKG_TRANSPORT_FRAGMENT_COUNT_MAX)
# Link自身物理发送批次仍使用LINKG_LINK_TX_BATCH_SIZE_DEFAULT。

# handler unregister接口注释补充生命周期契约：
# 调用方必须先停止可能进入对应handler的数据面，再释放user_data；RX会在g_transport.lock外调用已复制回调。
