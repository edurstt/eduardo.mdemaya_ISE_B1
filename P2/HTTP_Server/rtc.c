#include <stdio.h>
#include <string.h>
#include <time.h>
#include "rtc.h"
#include "lcd.h"
#include "rl_net.h"

// Offset horario: +1h o +2h (7200s verano España)
#define UTC_OFFSET_SEC  7200U 

RTC_HandleTypeDef RtcHandle;
RTC_DateTypeDef   sdatestructure;
RTC_TimeTypeDef   stimestructure;

/* RTOS Resources */
static osThreadId_t tid_ThAlarm;
static osTimerId_t  tim_id_verde;
static osTimerId_t  tim_id_rojo;
static osTimerId_t  tim_id_3min;

#define FLAG_ALARM  0x01U
#define FLAG_SNTP   0x02U

static volatile int cnt_verde = 0;
static volatile int cnt_rojo  = 0;

/* Prototipos internos */
static void time_callback(uint32_t seconds, uint32_t seconds_fraction);

/* ------------------ CONFIGURACIÓN HARDWARE (HAL) ------------------ */

void HAL_RTC_MspInit(RTC_HandleTypeDef *hrtc)
{
    RCC_OscInitTypeDef       RCC_OscInitStruct   = {0};
    RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_LSE;
    RCC_OscInitStruct.LSEState       = RCC_LSE_ON;
    RCC_OscInitStruct.PLL.PLLState   = RCC_PLL_NONE;
    HAL_RCC_OscConfig(&RCC_OscInitStruct);

    PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_RTC;
    PeriphClkInitStruct.RTCClockSelection    = RCC_RTCCLKSOURCE_LSE;
    HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct);

    __HAL_RCC_RTC_ENABLE();
}

/* Configura la hora a 01/01/2000 y marca el Backup Register */
void RTC_CalendarConfig(void)
{
    sdatestructure.Year    = 0;
    sdatestructure.Month   = RTC_MONTH_JANUARY;
    sdatestructure.Date    = 1;
    sdatestructure.WeekDay = RTC_WEEKDAY_SATURDAY;
    HAL_RTC_SetDate(&RtcHandle, &sdatestructure, RTC_FORMAT_BIN);

    stimestructure.Hours          = 0;
    stimestructure.Minutes        = 0;
    stimestructure.Seconds        = 0;
    stimestructure.TimeFormat     = RTC_HOURFORMAT24;
    stimestructure.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    stimestructure.StoreOperation = RTC_STOREOPERATION_RESET;
    HAL_RTC_SetTime(&RtcHandle, &stimestructure, RTC_FORMAT_BIN);

    // Guardamos la firma para saber que el RTC ya se ha configurado
    HAL_RTCEx_BKUPWrite(&RtcHandle, RTC_BKP_DR1, 0x32F2);
}

void RTC_CalendarShow(uint8_t *showtime, uint8_t *showdate)
{
    HAL_RTC_GetTime(&RtcHandle, &stimestructure, RTC_FORMAT_BIN);
    HAL_RTC_GetDate(&RtcHandle, &sdatestructure, RTC_FORMAT_BIN);

    sprintf((char *)showtime, "%02d:%02d:%02d",
            stimestructure.Hours,
            stimestructure.Minutes,
            stimestructure.Seconds);

    sprintf((char *)showdate, "%02d-%02d-20%02d",
            sdatestructure.Date,
            sdatestructure.Month,
            sdatestructure.Year);
}

void RTC_AlarmConfig(void)
{
    RTC_AlarmTypeDef sAlarm = {0};

    sAlarm.AlarmTime.Hours          = 0x00;
    sAlarm.AlarmTime.Minutes        = 0x00;
    sAlarm.AlarmTime.Seconds        = 0x00;
    sAlarm.AlarmTime.TimeFormat     = RTC_HOURFORMAT24;
    sAlarm.AlarmTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    sAlarm.AlarmTime.StoreOperation = RTC_STOREOPERATION_RESET;

    /* Ignora Horas, Minutos y Fecha. Solo mira que Segundos == 00 */
    sAlarm.AlarmMask = RTC_ALARMMASK_HOURS | RTC_ALARMMASK_MINUTES | RTC_ALARMMASK_DATEWEEKDAY;
    sAlarm.AlarmSubSecondMask = RTC_ALARMSUBSECONDMASK_ALL;
    sAlarm.Alarm              = RTC_ALARM_A;

    HAL_NVIC_SetPriority(RTC_Alarm_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(RTC_Alarm_IRQn);

    HAL_RTC_SetAlarm_IT(&RtcHandle, &sAlarm, RTC_FORMAT_BIN);
}

/* ISR del hardware para la Alarma RTC */
void RTC_Alarm_IRQHandler(void)
{
    HAL_RTC_AlarmIRQHandler(&RtcHandle);
}

void HAL_RTC_AlarmAEventCallback(RTC_HandleTypeDef *hrtc)
{
    osThreadFlagsSet(tid_ThAlarm, FLAG_ALARM);
}

/* Configura el Botón Azul usando Interrupciones (EXTI15_10) */
void Pulsador_config(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOC_CLK_ENABLE();
    GPIO_InitStruct.Pin = GPIO_PIN_13;
    GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING; // Salta cuando se pulsa (va a Alto)
    GPIO_InitStruct.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    HAL_NVIC_SetPriority(EXTI15_10_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
}

/* ISR del Botón Azul */
void EXTI15_10_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_13);
}

/* Qué hacer cuando se pulsa el Botón Azul */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if(GPIO_Pin == GPIO_PIN_13)
    {
        /* Vuelve a 01/01/2000 */
        RTC_CalendarConfig();
        
        /* Detiene el timer de 3 min si estaba en marcha y lo reinicia desde cero */
        osTimerStop(tim_id_3min);
        osTimerStart(tim_id_3min, 180000U); // 3 minutos
    }
}

/* ------------------ RTOS TIMERS Y CALLBACKS ------------------ */

static void Timer_Callback_Verde(void *arg)
{
    HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_0); // Toggle Verde (PB0)
    if (++cnt_verde >= 10) // 5 Segundos (10 toggle a 500ms)
    {
        cnt_verde = 0;
        osTimerStop(tim_id_verde);
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_RESET); // Apagar
    }
}

static void Timer_Callback_Rojo(void *arg)
{
    HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_14); // Toggle Rojo (PB14)
    if (++cnt_rojo >= 40) // 4 Segundos a 5Hz (40 toggle a 100ms)
    {
        cnt_rojo = 0;
        osTimerStop(tim_id_rojo);
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_RESET); // Apagar
    }
}

static void Timer_Callback_3min(void *arg)
{
    init_SNTP();
}

static void ThAlarm(void *argument)
{
    while (1)
    {
        /* Espera a que salte la Alarma A del RTC */
        osThreadFlagsWait(FLAG_ALARM, osFlagsWaitAny, osWaitForever);
        cnt_verde = 0;
        osTimerStart(tim_id_verde, 500U); // Inicia parpadeo verde
    }
}

/* ------------------ SNTP Y RED ------------------ */

static void time_callback(uint32_t seconds, uint32_t seconds_fraction)
{
    struct tm      *ts;
    time_t          t;
    RTC_DateTypeDef sDate = {0};
    RTC_TimeTypeDef sTime = {0};

    if (seconds == 0U) return;

    seconds += UTC_OFFSET_SEC; 
    t  = (time_t)seconds;
    ts = gmtime(&t);
    
    if (ts == NULL) return;

    sDate.Year    = (uint8_t)(ts->tm_year - 100);
    sDate.Month   = (uint8_t)(ts->tm_mon  +   1);
    sDate.Date    = (uint8_t)(ts->tm_mday);
    sDate.WeekDay = (uint8_t)(ts->tm_wday == 0 ? 7 : ts->tm_wday);

    sTime.Hours          = (uint8_t)(ts->tm_hour);
    sTime.Minutes        = (uint8_t)(ts->tm_min);
    sTime.Seconds        = (uint8_t)(ts->tm_sec);
    sTime.TimeFormat     = RTC_HOURFORMAT24;
    sTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    sTime.StoreOperation = RTC_STOREOPERATION_RESET;

    HAL_RTC_SetDate(&RtcHandle, &sDate, RTC_FORMAT_BIN);
    HAL_RTC_SetTime(&RtcHandle, &sTime, RTC_FORMAT_BIN);
    
    /* La hora de internet se ha guardado, guardamos la firma */
    HAL_RTCEx_BKUPWrite(&RtcHandle, RTC_BKP_DR1, 0x32F2);

    /* Arrancar el parpadeo del LED Rojo */
    cnt_rojo = 0;
    osTimerStart(tim_id_rojo, 100U);
}

void init_SNTP(void)
{
    /* Enviar petición SNTP. Usamos servidor ntp por defecto (NULL) */
    netSNTPc_GetTime(NULL, time_callback);
}

/* ------------------ FUNCIÓN PRINCIPAL RTC ------------------ */

void init_RTC(void)
{
    /* 1. Inicializar periferico RTC */
    RtcHandle.Instance          = RTC;
    RtcHandle.Init.HourFormat   = RTC_HOURFORMAT_24;
    RtcHandle.Init.AsynchPrediv = 127;
    RtcHandle.Init.SynchPrediv  = 255;
    __HAL_RTC_RESET_HANDLE_STATE(&RtcHandle);
    HAL_RTC_Init(&RtcHandle);

    /* 2. Comprobar si ya estaba configurado antes del Reset (botón negro) */
    if (HAL_RTCEx_BKUPRead(&RtcHandle, RTC_BKP_DR1) != 0x32F2)
    {
        /* Si es el primer arranque, poner a 01/01/2000 */
        RTC_CalendarConfig();
    }

    /* 3. Crear timers OS (OJO: no auto-start) */
    tim_id_verde = osTimerNew(Timer_Callback_Verde, osTimerPeriodic, NULL, NULL);
    tim_id_rojo  = osTimerNew(Timer_Callback_Rojo,  osTimerPeriodic, NULL, NULL);
    tim_id_3min  = osTimerNew(Timer_Callback_3min,  osTimerPeriodic, NULL, NULL);

    /* 4. Hilo de Alarma y Botón Azul */
    tid_ThAlarm = osThreadNew(ThAlarm, NULL, NULL);
    Pulsador_config(); // Botón azul gestionado por interrupciones

    /* 5. Configurar alarma RTC cada minuto */
    RTC_AlarmConfig();

    /* 6. Primera sincronizacion SNTP (El delay de 5s ya se hace en app_main) */
    init_SNTP();

    /* 7. Re-sincronizacion automatica cada 3 minutos */
    osTimerStart(tim_id_3min, 180000U);
}