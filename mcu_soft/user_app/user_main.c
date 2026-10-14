/*
增加中断检测口
*/
#include "main.h"
#include "stdbool.h"
#define  get_cpuBeep()  HAL_GPIO_ReadPin(CPU_BEEP_GPIO_Port,CPU_BEEP_Pin)
#define  get_sfModel()  HAL_GPIO_ReadPin(SF_MODEL_GPIO_Port,SF_MODEL_Pin)
#define  get_mcuModel() HAL_GPIO_ReadPin(MCU_MODEL_GPIO_Port,MCU_MODEL_Pin)//1为sbus,0为关闭sbus
#define  get_inV()      HAL_GPIO_ReadPin(PWR_IN_DETE_GPIO_Port,PWR_IN_DETE_Pin)
#define  get_key()      HAL_GPIO_ReadPin(KEY_WKUP_GPIO_Port,KEY_WKUP_Pin)
#define  set_5vOnOff(n)  HAL_GPIO_WritePin(PWR_5V_ONOFF_GPIO_Port,PWR_5V_ONOFF_Pin,(GPIO_PinState)(!n))
#define  set_powerOff(n) HAL_GPIO_WritePin(PWR_OFF_GPIO_Port,PWR_OFF_Pin,(GPIO_PinState)n)
#define  _get_charge_detec() (!HAL_GPIO_ReadPin(CHARGE_DETEC_GPIO_Port,CHARGE_DETEC_Pin))
static void reset_switch(void);
static uint16_t adc[1];
static uint8_t pmgModel = 0;
static uint8_t iwdogFlag = 0;

static uint16_t beepSet = 0;
static uint8_t key_onoff_detec = 0;
#define beepDiDelay 50
static void sound_onoff(void)
{
    beepSet = beepDiDelay;
    HAL_Delay(100);
    beepSet = beepDiDelay;
    HAL_Delay(100);
}
static void sound_key(void)
{
    beepSet = beepDiDelay;
}
// return: 0//没有充电，1//充电中，2//充满电
// 没充电 810hz，充满电=，充电中=
static uint32_t plushCnt = 0;
static uint8_t charge_detec_func(void)
{
    static int cnt = 0;
    static uint8_t chargflag = 0;

    if(++plushCnt >= 500)
    {
        chargflag = 1;
        cnt = 0;
    }

    if(++cnt > 2200)
    {
        chargflag = 0;
        cnt = 0;
    }

    return chargflag;
}

static uint8_t chargeFlag = 0;
__STATIC_INLINE uint32_t GXT_SYSTICK_IsActiveCounterFlag(void)
{
    return ((SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) == (SysTick_CTRL_COUNTFLAG_Msk));
}
//获取系统时间，单位us
static uint32_t getCurrentMicros(void)
{
    /* Ensure COUNTFLAG is reset by reading SysTick control and status register */
    GXT_SYSTICK_IsActiveCounterFlag();
    uint32_t m = HAL_GetTick();
    const uint32_t tms = SysTick->LOAD + 1;
    __IO uint32_t u = tms - SysTick->VAL;

    if (GXT_SYSTICK_IsActiveCounterFlag())
    {
        m = HAL_GetTick();
        u = tms - SysTick->VAL;
    }

    return (m * 1000 + (u * 1000) / tms);
}


void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if(GPIO_Pin == CHARGE_DETEC_Pin)
    {
        plushCnt = 0;
//		 static uint32_t hisTime=0;
//
//		 uint32_t val=getCurrentMicros()-hisTime;
//		 hisTime=getCurrentMicros();
//		 if(val>50)
//		 {
//			 plushCnt=0;
//		 }
    }
}
void beep_int(void)
{
    static uint16_t cnt = 0;
    static uint8_t runFlag = 0;
    //adc[0] = 4000;
    //chargeDetecFlag=get_charge_detec();
    chargeFlag = charge_detec_func();

    if(beepSet)
    {
        cnt = beepSet;
        beepSet = 0;

        if(runFlag == 0)  HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1);

        runFlag = 1;
    }

    if(runFlag)
    {
        cnt--;

        if(cnt == 0)
        {
            HAL_TIM_PWM_Stop(&htim2, TIM_CHANNEL_1);
            runFlag = 0;
        }
    }

    if(get_cpuBeep() && (pmgModel == 4))
    {
        beepSet = 2;
    }

    static __IO uint8_t keyCnt = 0, keyTouch = 0;
    static __IO   uint32_t keyPressTimeout = 0, keyUpTimeout = 0;

    if(get_key() && (keyTouch == 0))
    {
        sound_key();
        keyTouch = 1;
        keyCnt++;
        keyPressTimeout = 0;
        keyUpTimeout = 0;
    }
    else if(get_key() == 0)
    {
        keyTouch = 0;
        keyPressTimeout = 0;
        keyUpTimeout++;

        if(keyUpTimeout > 2000)
            keyCnt = 0;
    }

    if(get_key())
    {
        keyPressTimeout++;
        keyUpTimeout = 0;

        if((keyPressTimeout > 2000) && (keyCnt == 2))
        {

            key_onoff_detec = 1;
            keyCnt = keyPressTimeout = 0;
        }
    }

}
static void sbus_ctl(void)
{
    if(get_mcuModel())
        on_sbus();
    else
        off_sbus();
}


/*
按压两次并长按2s开机，同时电源指示灯或其他灯做逐步亮起交互
按压两次并长按2s关机，同时电源指示灯或其他灯做逐步熄灭交互

开关机音效；  滴滴~
配对音效，配对成功音效； 滴~
低电量警报，滴滴滴滴滴~5s~滴滴滴滴滴~...
失联警报；滴滴~1s~滴滴~1s~滴滴~1s~...
满电警报。滴~~~~~滴~30s~滴~~~~~滴~...
*/
static uint8_t model_start(uint8_t hisModel)
{
    if(get_inV() && (get_key() == 0)) return 1; //充电模式

    if(get_key() == 0) return 5;

    uint32_t timer = HAL_GetTick();
    //return 2;
//    while(get_key())
//    {
//        if((HAL_GetTick() - timer) > 1000) return 3;
//    }
    timer = HAL_GetTick();

    //beepSet=50;
    while(1)
    {
        if(get_key())
        {
            if((HAL_GetTick() - timer) > 2000)
            {
                //sound_onoff();
                return 2;
            }
        }
        else
        {
            if((HAL_GetTick() - timer) > 2000) return 5;

        }
    }

}

static void sound_lowPower(void)
{
    static	uint32_t histime = 0;
    static  uint8_t runCnt = 0;

    if((HAL_GetTick() - histime) > 5000)
    {
        histime = HAL_GetTick();
        runCnt = 5;
    }

    if(runCnt == 0) return;

    if((HAL_GetTick() - histime) > 500)
    {
        beepSet = beepDiDelay;
        histime = HAL_GetTick();
        runCnt--;
    }

}

#define _valtLower 20//电池下限 百分比
#define _valtLowerOff 1//电池下限关机阈值，百分比
#define _batthc    (_valtLower+3) //回差值
#define _batK       1.1037f //1.1826f
#define _liBatLow   ((int32_t)(3150.0f/_batK)) //3400.0fmv
#define _liBatHi    ((int32_t)(3900.0f/_batK)) // 
#define _detecChargVolat  (4000.0f/_batK)
#define _detecChargVolatHc  (4180.0f/_batK) //回差电压
uint8_t led_power_libatt(void)
{

    static  uint8_t val = 0, chrgFlag = 0;
    static uint32_t sampleTime = 0, samplesum = 0;
    static uint8_t cnt = 0;
    static uint16_t adcavge = 0;

    if((HAL_GetTick() - sampleTime) > 20)
    {
        sampleTime = HAL_GetTick();
        samplesum += adc[0];
        cnt++;

        if(cnt >= 50)
        {
            adcavge = samplesum / cnt;
            cnt = samplesum = 0;
            //printf("bat:%u\r\n",(int)((float)adcavge*_batK));

        }

        int32_t val32 = 0;

        if(chrgFlag)
        {
            val32 = ((((int32_t)adcavge - _liBatLow) * 1000) / (_detecChargVolatHc - _liBatLow)) / 10;
        }
        else
            val32 = ((((int32_t)adcavge - _liBatLow) * 1000) / (_liBatHi - _liBatLow)) / 10;

        if(val32 < 0) val32 = 0;
        else if(val32 > 100) val32 = 100;

        val = val32 / 33 + 1;

        if(chargeFlag == 1) chrgFlag = 1;
        else chrgFlag = 0;

//        if(get_inV() && (adcavge <= _detecChargVolat))
//        {
//            //在充电
//            chrgFlag = 1;
//        }
//        else if(get_inV() && (adcavge >= _detecChargVolatHc))
//        {
//            //充满
//            chrgFlag = 0;
//        }
//        else if(get_inV() == 0)
//        {
//            //没有充电
//            chrgFlag = 0;
//        }

    }



    static bool ledSyncFlag = 0;
    static uint32_t histime = 0;

    if((HAL_GetTick() - histime) > 500)
    {
        histime = HAL_GetTick();
        ledSyncFlag = !ledSyncFlag;
    }

    static uint8_t hisval = 0;
    static uint32_t hiscnt = 0; //10ms

    if(val != hisval)
    {
        if(++hiscnt > 200)
        {
            hisval = val;
            hiscnt = 0;
        }
    }
    else
    {
        hiscnt = 0;

    }

    switch(hisval)
    {
        case 1:
            if(chrgFlag == 0)
            {
                sound_lowPower();
            }

            HAL_GPIO_WritePin(LED_PWR1_GPIO_Port, LED_PWR1_Pin, (GPIO_PinState)ledSyncFlag);
            HAL_GPIO_WritePin(LED_PWR2_GPIO_Port, LED_PWR2_Pin, (GPIO_PinState)1);
            HAL_GPIO_WritePin(LED_PWR3_GPIO_Port, LED_PWR3_Pin, (GPIO_PinState)1);
            HAL_GPIO_WritePin(LED_PWR4_GPIO_Port, LED_PWR4_Pin, (GPIO_PinState)1);
            break;

        case 2:
            if(chrgFlag == 1)
            {
                HAL_GPIO_WritePin(LED_PWR1_GPIO_Port, LED_PWR1_Pin, (GPIO_PinState)ledSyncFlag);
                HAL_GPIO_WritePin(LED_PWR2_GPIO_Port, LED_PWR2_Pin, (GPIO_PinState)ledSyncFlag);
            }
            else
            {
                HAL_GPIO_WritePin(LED_PWR1_GPIO_Port, LED_PWR1_Pin, (GPIO_PinState)0);
                HAL_GPIO_WritePin(LED_PWR2_GPIO_Port, LED_PWR2_Pin, (GPIO_PinState)0);
            }

            HAL_GPIO_WritePin(LED_PWR3_GPIO_Port, LED_PWR3_Pin, (GPIO_PinState)1);
            HAL_GPIO_WritePin(LED_PWR4_GPIO_Port, LED_PWR4_Pin, (GPIO_PinState)1);
            break;

        case 3:
            if(chrgFlag == 1)
            {
                HAL_GPIO_WritePin(LED_PWR1_GPIO_Port, LED_PWR1_Pin, (GPIO_PinState)ledSyncFlag);
                HAL_GPIO_WritePin(LED_PWR2_GPIO_Port, LED_PWR2_Pin, (GPIO_PinState)ledSyncFlag);
                HAL_GPIO_WritePin(LED_PWR3_GPIO_Port, LED_PWR3_Pin, (GPIO_PinState)ledSyncFlag);
            }
            else
            {
                HAL_GPIO_WritePin(LED_PWR1_GPIO_Port, LED_PWR1_Pin, (GPIO_PinState)0);
                HAL_GPIO_WritePin(LED_PWR2_GPIO_Port, LED_PWR2_Pin, (GPIO_PinState)0);
                HAL_GPIO_WritePin(LED_PWR3_GPIO_Port, LED_PWR3_Pin, (GPIO_PinState)0);
            }

            HAL_GPIO_WritePin(LED_PWR4_GPIO_Port, LED_PWR4_Pin, (GPIO_PinState)1);
            break;

        case 4:
            if(chrgFlag == 1)
            {
                HAL_GPIO_WritePin(LED_PWR1_GPIO_Port, LED_PWR1_Pin, (GPIO_PinState)ledSyncFlag);
                HAL_GPIO_WritePin(LED_PWR2_GPIO_Port, LED_PWR2_Pin, (GPIO_PinState)ledSyncFlag);
                HAL_GPIO_WritePin(LED_PWR3_GPIO_Port, LED_PWR3_Pin, (GPIO_PinState)ledSyncFlag);
                HAL_GPIO_WritePin(LED_PWR4_GPIO_Port, LED_PWR4_Pin, (GPIO_PinState)ledSyncFlag);
            }
            else
            {
                HAL_GPIO_WritePin(LED_PWR1_GPIO_Port, LED_PWR1_Pin, (GPIO_PinState)0);
                HAL_GPIO_WritePin(LED_PWR2_GPIO_Port, LED_PWR2_Pin, (GPIO_PinState)0);
                HAL_GPIO_WritePin(LED_PWR3_GPIO_Port, LED_PWR3_Pin, (GPIO_PinState)0);
                HAL_GPIO_WritePin(LED_PWR4_GPIO_Port, LED_PWR4_Pin, (GPIO_PinState)0);

            }

            break;

        case 5:
            if(chrgFlag == 1)
            {
                HAL_GPIO_WritePin(LED_PWR1_GPIO_Port, LED_PWR1_Pin, (GPIO_PinState)1);
                HAL_GPIO_WritePin(LED_PWR2_GPIO_Port, LED_PWR2_Pin, (GPIO_PinState)1);
                HAL_GPIO_WritePin(LED_PWR3_GPIO_Port, LED_PWR3_Pin, (GPIO_PinState)1);
                HAL_GPIO_WritePin(LED_PWR4_GPIO_Port, LED_PWR4_Pin, (GPIO_PinState)1);
            }

            break;

        default:
            break;
    }

    return chrgFlag;
}
#define disp_k (100.0f/_batK)
void disp_libatt(void)
{
    uint8_t val = 0, chrgFlag = 0;
    uint16_t adcavge = 0;
    adcavge = adc[0] - (uint16_t)disp_k;
    int32_t val32 = 0;

    if(chrgFlag)
        val32 = ((((int32_t)adcavge - _liBatLow) * 1000) / (_detecChargVolatHc - _liBatLow)) / 10;
    else
        val32 = ((((int32_t)adcavge - _liBatLow) * 1000) / (_liBatHi - _liBatLow)) / 10;

    if(val32 < 0) val32 = 0;
    else if(val32 > 100) val32 = 100;

    val = val32 / 33 + 1;


    switch(val)
    {
        case 1:
            HAL_GPIO_WritePin(LED_PWR1_GPIO_Port, LED_PWR1_Pin, (GPIO_PinState)0);
            HAL_GPIO_WritePin(LED_PWR2_GPIO_Port, LED_PWR2_Pin, (GPIO_PinState)1);
            HAL_GPIO_WritePin(LED_PWR3_GPIO_Port, LED_PWR3_Pin, (GPIO_PinState)1);
            HAL_GPIO_WritePin(LED_PWR4_GPIO_Port, LED_PWR4_Pin, (GPIO_PinState)1);
            break;

        case 2:

            HAL_GPIO_WritePin(LED_PWR1_GPIO_Port, LED_PWR1_Pin, (GPIO_PinState)0);
            HAL_GPIO_WritePin(LED_PWR2_GPIO_Port, LED_PWR2_Pin, (GPIO_PinState)0);
            HAL_GPIO_WritePin(LED_PWR3_GPIO_Port, LED_PWR3_Pin, (GPIO_PinState)1);
            HAL_GPIO_WritePin(LED_PWR4_GPIO_Port, LED_PWR4_Pin, (GPIO_PinState)1);
            break;

        case 3:

            HAL_GPIO_WritePin(LED_PWR1_GPIO_Port, LED_PWR1_Pin, (GPIO_PinState)0);
            HAL_GPIO_WritePin(LED_PWR2_GPIO_Port, LED_PWR2_Pin, (GPIO_PinState)0);
            HAL_GPIO_WritePin(LED_PWR3_GPIO_Port, LED_PWR3_Pin, (GPIO_PinState)0);
            HAL_GPIO_WritePin(LED_PWR4_GPIO_Port, LED_PWR4_Pin, (GPIO_PinState)1);
            break;

        case 4:
            HAL_GPIO_WritePin(LED_PWR1_GPIO_Port, LED_PWR1_Pin, (GPIO_PinState)0);
            HAL_GPIO_WritePin(LED_PWR2_GPIO_Port, LED_PWR2_Pin, (GPIO_PinState)0);
            HAL_GPIO_WritePin(LED_PWR3_GPIO_Port, LED_PWR3_Pin, (GPIO_PinState)0);
            HAL_GPIO_WritePin(LED_PWR4_GPIO_Port, LED_PWR4_Pin, (GPIO_PinState)0);
            break;

        default:
            break;
    }
}


static uint8_t model_on(uint8_t hismodel)
{
    sound_onoff();

    while(get_key())
    {
        HAL_Delay(10);
    }

    set_5vOnOff(GPIO_PIN_SET);//打开5V输出
    HAL_Delay(500);
    MX_IWDG_Init();
    iwdogFlag = 1;
    return 4;

}
static uint8_t model_charge(uint8_t hismodel)
{
    while(1)
    {
        led_power_libatt();

        if(key_onoff_detec)
        {
            key_onoff_detec = 0;
            return 2;
        }

        if(get_inV() == 0)
        {
            return 3;
        }
    }
}
static uint8_t model_off(uint8_t hismodel)
{
    while(1)
    {
        sound_onoff();
        set_5vOnOff(0);
        set_powerOff(0);
        HAL_Delay(80);
        set_powerOff(1);
        HAL_Delay(100);
        set_powerOff(0);
        HAL_Delay(80);
        set_powerOff(1);
        HAL_NVIC_SystemReset();
        //HAL_Delay(2000);
    }

}
static uint8_t model_off_noaudio(uint8_t hismodel)
{
    while(1)
    {
        set_5vOnOff(0);
        set_powerOff(0);
        HAL_Delay(80);
        set_powerOff(1);
        HAL_Delay(100);
        set_powerOff(0);
        HAL_Delay(80);
        set_powerOff(1);
        HAL_NVIC_SystemReset();
        //HAL_Delay(2000);
    }

}
static uint8_t hisModel = 0;
static void power_mg(void)
{
    if(get_sfModel()) return;

//********地面端*****************
    switch(pmgModel)
    {
        case 0:  //第一次启动
            pmgModel = model_start(hisModel);
            hisModel = 0;
            break;

        case 1:  //充电未开机，显示充电状态
            pmgModel = model_charge(hisModel);
            hisModel = 1;
            break;

        case 2:  //手动开机
            pmgModel = model_on(hisModel);
            hisModel = 2;
            key_onoff_detec = 0;
            break;

        case 3:  //关机
            pmgModel = model_off(hisModel);
            hisModel = 3;
            break;

        case 4:  //正常运行
            reset_switch();
            led_power_libatt();

            if(key_onoff_detec)  pmgModel = 3;

            break;

        case 5:  //点击一次模式
            model_off_noaudio(hisModel);
            hisModel = 5;
            break;

        default:
            break;
    }

}
static void user_protection(void)
{
    FLASH_OBProgramInitTypeDef OptionsBytesStruct;

    HAL_FLASHEx_OBGetConfig(&OptionsBytesStruct);

    //
    if(OptionsBytesStruct.RDPLevel == OB_RDP_LEVEL_0)
    {
        HAL_FLASH_Unlock();
        HAL_FLASH_OB_Unlock();
        //printf("OB_RDP_LEVEL_0:%d\r\n", OptionsBytesStruct.RDPLevel);
        /* Enable the pages write protection **************************************/
        OptionsBytesStruct.OptionType = OPTIONBYTE_RDP;
        //OptionsBytesStruct.WRPState   = OB_WRPSTATE_ENABLE;
        //OptionsBytesStruct.WRPPage    = FLASH_PAGE_TO_BE_PROTECTED;
        OptionsBytesStruct.RDPLevel = OB_RDP_LEVEL_1;

        if(HAL_FLASHEx_OBProgram(&OptionsBytesStruct) != HAL_OK)
        {
            /* Error occurred while options bytes programming. **********************/

        }

        HAL_FLASH_Lock();    /* 锁定FLASH  */
        HAL_FLASH_OB_Lock(); /* 锁定OPTION */
        /* 产生一个复位，option byte装载 */
        HAL_FLASH_OB_Launch();
    }

}
static void reset_switch(void)
{
    static uint32_t time = 0;
    static uint8_t flag = 0;

    if(time == 0) time = HAL_GetTick();

    if(((HAL_GetTick() - time) > 1000) && (flag == 0))
    {
        flag = 1;
        HAL_GPIO_WritePin(SWITCH_RESET_GPIO_Port, SWITCH_RESET_Pin, GPIO_PIN_RESET);
        HAL_Delay(50);
        HAL_GPIO_WritePin(SWITCH_RESET_GPIO_Port, SWITCH_RESET_Pin, GPIO_PIN_SET);
    }

}
static void reset_switch_one(void)
{
    HAL_GPIO_WritePin(SWITCH_RESET_GPIO_Port, SWITCH_RESET_Pin, GPIO_PIN_RESET);
    HAL_Delay(50);
    HAL_GPIO_WritePin(SWITCH_RESET_GPIO_Port, SWITCH_RESET_Pin, GPIO_PIN_SET);
}
#define hw_ver "1.1.0"
#define sf_ver "1.1.2"
void user_main(void)
{
    //user_protection();

    HAL_Delay(200);

    if(get_sfModel() == 0)
    {
        //地面端初始化
        HAL_ADCEx_Calibration_Start(&hadc, 0);
        HAL_ADC_Start_DMA(&hadc, (uint32_t*)adc, 1);
    }
    else
    {
        reset_switch_one();
        MX_IWDG_Init();
        iwdogFlag = 1;
    }

    HAL_Delay(100);
    disp_libatt();
    sound_key();
    HAL_Delay(800);
    pmgModel = 0;

    while(1)
    {
        //adc[0] = 4000;
        power_mg();//电源管理
        sbus_ctl();//sbus控制
        HAL_Delay(10);

        if(iwdogFlag == 1) HAL_IWDG_Refresh(&hiwdg);

    }


}
