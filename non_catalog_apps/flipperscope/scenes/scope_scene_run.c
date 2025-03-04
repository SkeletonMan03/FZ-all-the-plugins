#include <complex.h>
#include <math.h>

#include <float.h>
#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_resources.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/widget.h>
#include <gui/elements.h>
#include <notification/notification_messages.h>

#include "stm32wbxx_ll_adc.h"
#include "stm32wbxx_ll_dma.h"
#include "stm32wbxx_ll_crs.h"
#include "stm32wbxx_ll_rcc.h"
#include "stm32wbxx_ll_bus.h"
#include "stm32wbxx_ll_system.h"
#include "stm32wbxx_ll_exti.h"
#include "stm32wbxx_ll_cortex.h"
#include "stm32wbxx_ll_utils.h"
#include "stm32wbxx_ll_pwr.h"
#include "stm32wbxx_ll_tim.h"
#include "stm32wbxx_ll_gpio.h"

#include "../scope_app_i.h"
#include "flipperscope_icons.h"

#define USE_TIMEOUT                          0
#define DIGITAL_SCALE_12BITS                 ((uint32_t)0xFFF)
#define VAR_CONVERTED_DATA_INIT_VALUE        (DIGITAL_SCALE_12BITS + 1)
#define VAR_CONVERTED_DATA_INIT_VALUE_16BITS (0xFFFF + 1U)
#define __ADC_CALC_DATA_VOLTAGE(__VREFANALOG_VOLTAGE__, __ADC_DATA__) \
    ((__ADC_DATA__) * (__VREFANALOG_VOLTAGE__) / DIGITAL_SCALE_12BITS)
#define VDDA_APPLI                        ((uint32_t)2500)
#define TIMER_FREQUENCY_RANGE_MIN         (1UL)
#define TIMER_PRESCALER_MAX_VALUE         (0xFFFF - 1UL)
#define ADC_DELAY_CALIB_ENABLE_CPU_CYCLES (LL_ADC_DELAY_CALIB_ENABLE_ADC_CYCLES * 32)

// ramVector found from - https://community.nxp.com/t5/i-MX-Processors/Relocate-vector-table-to-ITCM/m-p/1302304
// the aligned aspect is key!
#define TABLE_SIZE 79
uint32_t ramVector[TABLE_SIZE + 1] __attribute__((aligned(512)));

const uint32_t AHBPrescTable[16UL] =
    {1UL, 3UL, 5UL, 1UL, 1UL, 6UL, 10UL, 32UL, 2UL, 4UL, 8UL, 16UL, 64UL, 128UL, 256UL, 512UL};
const uint32_t APBPrescTable[8UL] = {0UL, 0UL, 0UL, 0UL, 1UL, 2UL, 3UL, 4UL};
const uint32_t MSIRangeTable[16UL] = {
    100000UL,
    200000UL,
    400000UL,
    800000UL,
    1000000UL,
    2000000UL,
    4000000UL,
    8000000UL,
    16000000UL,
    24000000UL,
    32000000UL,
    48000000UL,
    0UL,
    0UL,
    0UL,
    0UL}; /* 0UL values are incorrect cases */

char* time; // Current time period text
float scale; // Current scale
double freq; // Current samplerate
uint8_t pause = 0; // Whether we want to pause output or not
enum measureenum type; // Type of measurement we are performing
int toggle = 0; // Used for toggling output GPIO, only used in testing
uint32_t adc_buffer; // ADC buffer size
int16_t* index_crossings; // Indexes of zero crossings
float* data; // Shift data across virtual zero line
float* crossings;
float complex* fft_data; // Real data, transformed to complex data via FFT
float* fft_power; // Power data from FFT

void Error_Handler() {
    while(1) {
    }
}

uint16_t* aADCxConvertedData; // Array that ADC data is copied to, via DMA
__IO uint16_t* aADCxConvertedData_Voltage_mVoltA; // Data is converted to range from 0 to 2500
__IO uint16_t* aADCxConvertedData_Voltage_mVoltB; // Data is converted to range from 0 to 2500
__IO uint8_t ubDmaTransferStatus = 2; // DMA transfer status

__IO uint16_t* mvoltWrite; // Pointer to area we write converted voltage data to
__IO uint16_t* mvoltDisplay; // Pointer to area of memory we display

void AdcDmaTransferComplete_Callback();
void AdcDmaTransferHalf_Callback();

void AdcGrpRegularOverrunError_Callback(void) {
    LL_ADC_DisableIT_OVR(ADC1);
}

void AdcDmaTransferError_Callback() {
}

void DMA1_Channel1_IRQHandler(void) {
    if(LL_DMA_IsActiveFlag_TC1(DMA1) == 1) {
        LL_DMA_ClearFlag_TC1(DMA1);
        AdcDmaTransferComplete_Callback();
    }

    if(LL_DMA_IsActiveFlag_HT1(DMA1) == 1) {
        LL_DMA_ClearFlag_HT1(DMA1);
        AdcDmaTransferHalf_Callback();
    }

    if(LL_DMA_IsActiveFlag_TE1(DMA1) == 1) {
        LL_DMA_ClearFlag_TE1(DMA1);
        AdcDmaTransferError_Callback();
    }
}

void ADC1_IRQHandler(void) {
    if(LL_ADC_IsActiveFlag_OVR(ADC1) != 0) {
        LL_ADC_ClearFlag_OVR(ADC1);
        AdcGrpRegularOverrunError_Callback();
    }
}

void TIM2_IRQHandler(void) {
}

static void MX_ADC1_Init(void) {
    LL_ADC_CommonInitTypeDef ADC_CommonInitStruct = {0};
    LL_ADC_InitTypeDef ADC_InitStruct = {0};
    LL_ADC_REG_InitTypeDef ADC_REG_InitStruct = {0};
    LL_GPIO_InitTypeDef GPIO_InitStruct = {0};

    LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_ADC);
    LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOC);
    GPIO_InitStruct.Pin = LL_GPIO_PIN_0;
    GPIO_InitStruct.Mode = LL_GPIO_MODE_ANALOG;
    GPIO_InitStruct.Pull = LL_GPIO_PULL_NO;
    LL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    LL_DMA_SetPeriphRequest(DMA1, LL_DMA_CHANNEL_1, LL_DMAMUX_REQ_ADC1);
    LL_DMA_SetDataTransferDirection(DMA1, LL_DMA_CHANNEL_1, LL_DMA_DIRECTION_PERIPH_TO_MEMORY);
    LL_DMA_SetChannelPriorityLevel(DMA1, LL_DMA_CHANNEL_1, LL_DMA_PRIORITY_HIGH);
    LL_DMA_SetMode(DMA1, LL_DMA_CHANNEL_1, LL_DMA_MODE_CIRCULAR);
    LL_DMA_SetPeriphIncMode(DMA1, LL_DMA_CHANNEL_1, LL_DMA_PERIPH_NOINCREMENT);
    LL_DMA_SetMemoryIncMode(DMA1, LL_DMA_CHANNEL_1, LL_DMA_MEMORY_INCREMENT);
    LL_DMA_SetPeriphSize(DMA1, LL_DMA_CHANNEL_1, LL_DMA_PDATAALIGN_HALFWORD);
    LL_DMA_SetMemorySize(DMA1, LL_DMA_CHANNEL_1, LL_DMA_MDATAALIGN_HALFWORD);

    LL_DMAMUX_SetRequestID(DMAMUX1, LL_DMAMUX_CHANNEL_0, LL_DMAMUX_REQ_ADC1);

    LL_DMA_ConfigAddresses(
        DMA1,
        LL_DMA_CHANNEL_1,
        LL_ADC_DMA_GetRegAddr(ADC1, LL_ADC_DMA_REG_REGULAR_DATA),
        (uint32_t)aADCxConvertedData,
        LL_DMA_DIRECTION_PERIPH_TO_MEMORY);

    LL_DMA_SetDataLength(DMA1, LL_DMA_CHANNEL_1, adc_buffer);

    LL_DMA_EnableIT_TC(DMA1, LL_DMA_CHANNEL_1);
    LL_DMA_EnableIT_HT(DMA1, LL_DMA_CHANNEL_1);
    LL_DMA_EnableIT_TE(DMA1, LL_DMA_CHANNEL_1);

    LL_DMA_EnableChannel(DMA1, LL_DMA_CHANNEL_1);
    NVIC_SetPriority(ADC1_IRQn, 0);
    NVIC_EnableIRQ(ADC1_IRQn);

    ADC_CommonInitStruct.CommonClock = LL_ADC_CLOCK_SYNC_PCLK_DIV2;
    LL_ADC_CommonInit(__LL_ADC_COMMON_INSTANCE(ADC1), &ADC_CommonInitStruct);
    ADC_InitStruct.Resolution = LL_ADC_RESOLUTION_12B;
    ADC_InitStruct.DataAlignment = LL_ADC_DATA_ALIGN_RIGHT;
    ADC_InitStruct.LowPowerMode = LL_ADC_LP_MODE_NONE;
    LL_ADC_Init(ADC1, &ADC_InitStruct);
    ADC_REG_InitStruct.TriggerSource = LL_ADC_REG_TRIG_EXT_TIM2_TRGO;
    ADC_REG_InitStruct.SequencerLength = LL_ADC_REG_SEQ_SCAN_DISABLE;
    ADC_REG_InitStruct.SequencerDiscont = LL_ADC_REG_SEQ_DISCONT_DISABLE;
    ADC_REG_InitStruct.ContinuousMode = LL_ADC_REG_CONV_SINGLE;
    ADC_REG_InitStruct.DMATransfer = LL_ADC_REG_DMA_TRANSFER_UNLIMITED;
    ADC_REG_InitStruct.Overrun = LL_ADC_REG_OVR_DATA_OVERWRITTEN;
    LL_ADC_REG_Init(ADC1, &ADC_REG_InitStruct);
    LL_ADC_SetOverSamplingScope(ADC1, LL_ADC_OVS_DISABLE);
    LL_ADC_REG_SetTriggerEdge(ADC1, LL_ADC_REG_TRIG_EXT_FALLING);

    LL_ADC_DisableDeepPowerDown(ADC1);
    LL_ADC_EnableInternalRegulator(ADC1);
    uint32_t wait_loop_index;
    wait_loop_index =
        ((LL_ADC_DELAY_INTERNAL_REGUL_STAB_US * (SystemCoreClock / (100000 * 2))) / 10);
    while(wait_loop_index != 0) {
        wait_loop_index--;
    }

    LL_ADC_REG_SetSequencerRanks(ADC1, LL_ADC_REG_RANK_1, LL_ADC_CHANNEL_1);
    LL_ADC_SetChannelSamplingTime(ADC1, LL_ADC_CHANNEL_1, LL_ADC_SAMPLINGTIME_247CYCLES_5);
    LL_ADC_SetChannelSingleDiff(ADC1, LL_ADC_CHANNEL_1, LL_ADC_SINGLE_ENDED);
    LL_ADC_EnableIT_OVR(ADC1);
}

double abs_error(double num1, double num2) {
    return fabs((num1 - num2) / num1);
}

static void MX_TIM2_Init(int freq) {
    uint32_t timer_clock_frequency = 0; /* Timer clock frequency */
    uint32_t timer_prescaler =
        0; /* Time base prescaler to have timebase aligned on minimum frequency possible */
    uint32_t timer_reload =
        0; /* Timer reload value in function of timer prescaler to achieve time base period */

    LL_TIM_InitTypeDef TIM_InitStruct = {0};
    LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM2);

    if(LL_RCC_GetAPB1Prescaler() == LL_RCC_APB1_DIV_1) {
        timer_clock_frequency =
            __LL_RCC_CALC_PCLK1_FREQ(SystemCoreClock, LL_RCC_GetAPB1Prescaler());
    } else {
        timer_clock_frequency =
            (__LL_RCC_CALC_PCLK1_FREQ(SystemCoreClock, LL_RCC_GetAPB1Prescaler()) * 2);
    }

    //(PSC+1) * (ARR+1)
    double calc = timer_clock_frequency / (1 / (1 / (double)freq));
    double PSC;
    double ARR;
    double minerr = 10000;
    for(int i = 1; i < 65536; i++) {
        PSC = i - 1;
        ARR = calc / (PSC + 1);
        double error = abs_error((int)(ARR), ARR);
        if(error < (double)0.001 && error < minerr && ARR - 1 > 0) {
            timer_prescaler = PSC;
            timer_reload = ARR - 1;
            minerr = error;
            break;
        }
    }

    TIM_InitStruct.Prescaler = timer_prescaler;
    TIM_InitStruct.CounterMode = LL_TIM_COUNTERMODE_UP;
    TIM_InitStruct.Autoreload = timer_reload;
    TIM_InitStruct.ClockDivision = LL_TIM_CLOCKDIVISION_DIV1;
    LL_TIM_Init(TIM2, &TIM_InitStruct);
    LL_TIM_SetTriggerInput(TIM2, LL_TIM_TS_ITR0);
    LL_TIM_SetSlaveMode(TIM2, LL_TIM_SLAVEMODE_DISABLED);
    LL_TIM_DisableIT_TRIG(TIM2);
    LL_TIM_DisableDMAReq_TRIG(TIM2);
    LL_TIM_SetTriggerOutput(TIM2, LL_TIM_TRGO_UPDATE);
    LL_TIM_DisableMasterSlaveMode(TIM2);
    LL_TIM_SetTriggerOutput(TIM2, LL_TIM_TRGO_UPDATE);
    LL_TIM_EnableCounter(TIM2);
}

static void MX_DMA_Init(void) {
    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMAMUX1);
    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);
    NVIC_SetPriority(DMA1_Channel1_IRQn, NVIC_EncodePriority(NVIC_GetPriorityGrouping(), 1, 0));
    NVIC_EnableIRQ(DMA1_Channel1_IRQn);
}

static void MX_GPIO_Init(void) {
    LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOA);
    LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOB);
    LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOC);
}

// Swap pointer addresses, used for double buffer
void swap(__IO uint16_t** a, __IO uint16_t** b) {
    __IO uint16_t* tmp;
    tmp = *a;
    *a = *b;
    *b = tmp;
}

void AdcDmaTransferComplete_Callback() {
    uint32_t tmp_index = 0;
    for(tmp_index = (adc_buffer / 2); tmp_index < adc_buffer; tmp_index++) {
        mvoltWrite[tmp_index] = __LL_ADC_CALC_DATA_TO_VOLTAGE(
            VDDA_APPLI, aADCxConvertedData[tmp_index], LL_ADC_RESOLUTION_12B);
    }
    ubDmaTransferStatus = 1;
    if(!pause) swap(&mvoltWrite, &mvoltDisplay);
}

void AdcDmaTransferHalf_Callback() {
    uint32_t tmp_index = 0;
    for(tmp_index = 0; tmp_index < (adc_buffer / 2); tmp_index++) {
        mvoltWrite[tmp_index] = __LL_ADC_CALC_DATA_TO_VOLTAGE(
            VDDA_APPLI, aADCxConvertedData[tmp_index], LL_ADC_RESOLUTION_12B);
    }
    ubDmaTransferStatus = 0;
}

void Activate_ADC(void) {
    __IO uint32_t wait_loop_index = 0U;
#if(USE_TIMEOUT == 1)
    uint32_t Timeout = 0U; /* Variable used for timeout management */
#endif /* USE_TIMEOUT */
    if(LL_ADC_IsEnabled(ADC1) == 0) {
        LL_ADC_DisableDeepPowerDown(ADC1);
        LL_ADC_EnableInternalRegulator(ADC1);
        wait_loop_index =
            ((LL_ADC_DELAY_INTERNAL_REGUL_STAB_US * (SystemCoreClock / (100000 * 2))) / 10);
        while(wait_loop_index != 0) {
            wait_loop_index--;
        }
        LL_ADC_StartCalibration(ADC1, LL_ADC_SINGLE_ENDED);

#if(USE_TIMEOUT == 1)
        Timeout = ADC_CALIBRATION_TIMEOUT_MS;
#endif /* USE_TIMEOUT */

        while(LL_ADC_IsCalibrationOnGoing(ADC1) != 0) {
#if(USE_TIMEOUT == 1)
            if(LL_SYSTICK_IsActiveCounterFlag()) {
                if(Timeout-- == 0) {
                }
            }
#endif /* USE_TIMEOUT */
        }
        wait_loop_index = (ADC_DELAY_CALIB_ENABLE_CPU_CYCLES >> 1);
        while(wait_loop_index != 0) {
            wait_loop_index--;
        }
        LL_ADC_Enable(ADC1);
#if(USE_TIMEOUT == 1)
        Timeout = ADC_ENABLE_TIMEOUT_MS;
#endif /* USE_TIMEOUT */
        while(LL_ADC_IsActiveFlag_ADRDY(ADC1) == 0) {
#if(USE_TIMEOUT == 1)
            /* Check Systick counter flag to decrement the time-out value */
            if(LL_SYSTICK_IsActiveCounterFlag()) {
                if(Timeout-- == 0) {
                    /* Time-out occurred. Set LED to blinking mode */
                    LED_Blinking(LED_BLINK_ERROR);
                }
            }
#endif /* USE_TIMEOUT */
        }
    }
}

// Found from:
// https://www.algorithm-archive.org/contents/cooley_tukey/cooley_tukey.html
void bit_reverse(float complex* X, int N) {
    for(int i = 0; i < N; ++i) {
        int n = i;
        int a = i;
        int count = (int)ceil(log2((float)N)) - 1;

        n >>= 1;
        while(n > 0) {
            a = (a << 1) | (n & 1);
            count--;
            n >>= 1;
        }
        n = (a << count) & (int)((1 << (int)ceil(log2((float)N))) - 1);

        if(n > i) {
            float complex tmp = X[i];
            X[i] = X[n];
            X[n] = tmp;
        }
    }
}

// Found from:
// https://www.algorithm-archive.org/contents/cooley_tukey/cooley_tukey.html
//
// Adapted slightly to use ceil, otherwise didn't seem to calculate
// FFT correctly on the flipper zero
void iterative_cooley_tukey(float complex* X, int N) {
    bit_reverse(X, N);

    for(int i = 1; i <= ceil(log2((float)N)); ++i) {
        int stride = (int)pow(2, i);
        float complex w = cexp(-2.0 * I * M_PI / (float)stride);
        for(int j = 0; j < N; j += stride) {
            float complex v = 1.0;
            for(int k = 0; k < stride / 2; ++k) {
                X[k + j + stride / 2] = X[k + j] - v * X[k + j + stride / 2];
                X[k + j] -= (X[k + j + stride / 2] - X[k + j]);
                v *= w;
            }
        }
    }
}

// Found from:
// https://stackoverflow.com/questions/427477/fastest-way-to-clamp-a-real-fixed-floating-point-value
double clamp(double d, double min, double max) {
    const double t = d < min ? min : d;
    return t > max ? max : t;
}

// Used to draw to display
static void app_draw_callback(Canvas* canvas, void* ctx) {
    UNUSED(ctx);
    static char buf1[50];
    float max = 0.0;
    float min = FLT_MAX;
    int count = 0;

    if(type == m_capture) {
        if(!pause)
            elements_button_center(canvas, "Stop");
        else {
            elements_button_center(canvas, "REC");
            elements_button_right(canvas, "Save");
        }
    }

    if(pause)
        canvas_draw_icon(canvas, 116, 1, &I_pause_10x10);
    else
        canvas_draw_icon(canvas, 116, 1, &I_play_10x10);

    // Calculate voltage measurements
    for(uint32_t x = 0; x < adc_buffer; x++) {
        if(mvoltDisplay[x] < min) min = mvoltDisplay[x];
        if(mvoltDisplay[x] > max) max = mvoltDisplay[x];
    }
    max /= 1000;
    min /= 1000;

    switch(type) {
    case m_time: {
        // Display current scale
        snprintf(buf1, 50, "%.0fx", (double)scale);
        canvas_draw_str(canvas, 95, 10, buf1);
        // Display current time period
        snprintf(buf1, 50, "Time: %s", time);
        canvas_draw_str(canvas, 2, 10, buf1);
        // Shift waveform across a virtual 0 line, so it crosses 0
        for(uint32_t x = 0; x < adc_buffer; x++) {
            index_crossings[x] = -1;
            crossings[x] = -1.0;
            data[x] = ((float)mvoltDisplay[x] / 1000) - min;
            data[x] = ((2 / (max - min)) * data[x]) - 1;
        }
        // Find points at which waveform crosses virtual 0 line
        for(uint32_t x = 1; x < adc_buffer; x++) {
            if(data[x] >= 0 && data[x - 1] < 0) {
                index_crossings[count++] = x - 1;
            }
        }
        count = 0;
        // Linear interpolation to find zero crossings
        // see https://gist.github.com/endolith/255291 for Python version
        for(uint32_t x = 0; x < adc_buffer; x++) {
            if(index_crossings[x] == -1) break;
            crossings[count++] = (float)index_crossings[x] -
                                 data[index_crossings[x]] /
                                     (data[index_crossings[x] + 1] - data[index_crossings[x]]);
        }
        float avg = 0.0;
        float countv = 0.0;
        for(uint32_t x = 0; x < adc_buffer; x++) {
            if(x + 1 >= adc_buffer) break;
            if(crossings[x] == -1 || crossings[x + 1] == -1) break;
            avg += crossings[x + 1] - crossings[x];
            countv += 1;
        }
        avg /= countv;
        // Display frequency of waveform
        snprintf(buf1, 50, "Freq: %.1f Hz", (double)((float)freq / avg));
        canvas_draw_str(canvas, 2, 20, buf1);
    } break;
    case m_fft: {
        for(uint32_t i = 0; i < adc_buffer; i++) {
            fft_data[i] = ((float)mvoltDisplay[i] / 1000);
        }

        // Apply FFT
        iterative_cooley_tukey(fft_data, adc_buffer);

        // Find FFT bin, with highest power
        float max_val = -1;
        int idx = 0;
        for(uint32_t i = 1; i < adc_buffer / 2; i++) {
            float f = cabsf(fft_data[i]) * cabsf(fft_data[i]);
            if(f > max_val) {
                max_val = f;
                idx = i;
            }
            fft_power[i] = f;
        }

        // Display frequency of waveform
        snprintf(buf1, 50, "Freq: %.1fHz", (double)idx * ((double)freq / (double)adc_buffer));
        canvas_draw_str(canvas, 2, 10, buf1);
    } break;
    case m_voltage: {
        // Display current scale
        snprintf(buf1, 50, "%.0fx", (double)scale);
        canvas_draw_str(canvas, 95, 10, buf1);
        // Display max, min, peak-to-peak voltages
        snprintf(buf1, 50, "Max: %.2fV", (double)max);
        canvas_draw_str(canvas, 2, 10, buf1);
        snprintf(buf1, 50, "Min: %.2fV", (double)min);
        canvas_draw_str(canvas, 2, 20, buf1);
        snprintf(buf1, 50, "Vpp: %.2fV", (double)(max - min));
        canvas_draw_str(canvas, 2, 30, buf1);
    } break;
    default:
        break;
    }

    if(type != m_fft) {
        // Draw lines between each data point
        // y should range from 0 to 63
        for(uint32_t x = 1; x < adc_buffer; x++) {
            int32_t prev =
                63 - (uint32_t)(((float)mvoltDisplay[x - 1] / (float)VDDA_APPLI) * scale * 63.0f);
            int32_t cur =
                63 - (uint32_t)(((float)mvoltDisplay[x] / (float)VDDA_APPLI) * scale * 63.0f);
            if(!(prev < 0 && cur < 0))
                canvas_draw_line(canvas, x - 1, clamp(prev, 0, 63), x, clamp(cur, 0, 63));
        }
    } else {
        // Process FFT data - excluding bin 0
        float max = 0;
        for(uint32_t i = 1; i < adc_buffer / 2; i += adc_buffer / 2 / 128) {
            float sum = 0;
            for(uint32_t i2 = i; i2 < i + (adc_buffer / 2 / 128); i2++) {
                sum += fft_power[i2];
            }
            if(sum > max) {
                max = sum;
            }
        }

        uint32_t xpos = 0;
        // xpos: 0 to 126 for window size 256
        // xpos: 0 to 127 for window size 512
        // xpos: 0 to 127 for window size 1024
        // y should range from 0 to 63
        for(uint32_t i = 1; i < adc_buffer / 2; i += adc_buffer / 2 / 128) {
            float sum = 0;
            for(uint32_t i2 = i; i2 < i + (adc_buffer / 2 / 128); i2++) {
                sum += fft_power[i2];
            }
            canvas_draw_line(canvas, xpos, 63, xpos, 63 - (uint32_t)(((sum / max) * 63.0f)));
            xpos++;
        }
    }

    // Removing graph lines, to use extra pixel
    // Draw graph lines
    //canvas_draw_line(canvas, 0, 0, 0, 63);
    //canvas_draw_line(canvas, 0, 63, 127, 63);
}

static void app_input_callback(InputEvent* input_event, void* ctx) {
    furi_assert(ctx);
    FuriMessageQueue* event_queue = ctx;
    furi_message_queue_put(event_queue, input_event, FuriWaitForever);
}

// Free malloc'd data
void free_all() {
    free(aADCxConvertedData);
    free((void*)aADCxConvertedData_Voltage_mVoltA);
    free((void*)aADCxConvertedData_Voltage_mVoltB);
    free(index_crossings);
    free(data);
    free(crossings);
    free(fft_data);
    free(fft_power);
}

void scope_scene_run_on_enter(void* context) {
    ScopeApp* app = context;

    // Find string representation of time period we're using
    for(uint32_t i = 0; i < COUNT_OF(time_list); i++) {
        if(time_list[i].time == app->time) {
            time = time_list[i].str;
            break;
        }
    }

    // Obtain scale value
    scale = app->scale;

    // Currently un-paused
    pause = 0;

    // What type of measurement are we performing
    type = app->measurement;

    adc_buffer = ADC_CONVERTED_DATA_BUFFER_SIZE;
    if(type == m_fft) adc_buffer = app->fft;

    aADCxConvertedData = malloc(adc_buffer * sizeof(uint16_t));
    aADCxConvertedData_Voltage_mVoltA = malloc(adc_buffer * sizeof(uint16_t));
    aADCxConvertedData_Voltage_mVoltB = malloc(adc_buffer * sizeof(uint16_t));

    index_crossings = malloc(adc_buffer * sizeof(int16_t));
    data = malloc(adc_buffer * sizeof(float));
    crossings = malloc(adc_buffer * sizeof(float));
    fft_data = malloc(adc_buffer * sizeof(float complex));
    fft_power = malloc(adc_buffer * sizeof(float));

    mvoltWrite =
        &aADCxConvertedData_Voltage_mVoltA[0]; // Pointer to area we write converted voltage data to
    mvoltDisplay = &aADCxConvertedData_Voltage_mVoltB[0]; // Pointer to area of memory we display

    // Copy vector table, modify to use our own IRQ handlers
    __disable_irq();
    memcpy(ramVector, (uint32_t*)(FLASH_BASE | SCB->VTOR), sizeof(uint32_t) * TABLE_SIZE);
    SCB->VTOR = (uint32_t)ramVector;
    ramVector[27] = (uint32_t)DMA1_Channel1_IRQHandler;
    ramVector[34] = (uint32_t)ADC1_IRQHandler;
    ramVector[44] = (uint32_t)TIM2_IRQHandler;
    __enable_irq();

    furi_hal_bus_enable(FuriHalBusTIM2);

    FuriMessageQueue* event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));

    uint32_t tmp_index_adc_converted_data = 0;
    MX_GPIO_Init();
    MX_DMA_Init();

    freq = 1 / app->time;
    MX_TIM2_Init((int)freq);

    // Set VREFBUF to 2.5V, as vref isn't connected to 3.3V itself in the flipper zero
    VREFBUF->CSR |= VREFBUF_CSR_ENVR;
    VREFBUF->CSR &= ~VREFBUF_CSR_HIZ;
    VREFBUF->CSR |= VREFBUF_CSR_VRS;
    while(!(VREFBUF->CSR & VREFBUF_CSR_VRR)) {
    };

    MX_ADC1_Init();

    // Setup initial values from ADC
    for(tmp_index_adc_converted_data = 0; tmp_index_adc_converted_data < adc_buffer;
        tmp_index_adc_converted_data++) {
        aADCxConvertedData[tmp_index_adc_converted_data] = VAR_CONVERTED_DATA_INIT_VALUE;
        aADCxConvertedData_Voltage_mVoltA[tmp_index_adc_converted_data] = 0;
        aADCxConvertedData_Voltage_mVoltB[tmp_index_adc_converted_data] = 0;
    }

    Activate_ADC();

    if((LL_ADC_IsEnabled(ADC1) == 1) && (LL_ADC_IsDisableOngoing(ADC1) == 0) &&
       (LL_ADC_REG_IsConversionOngoing(ADC1) == 0)) {
        LL_ADC_REG_StartConversion(ADC1);
    }

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, app_draw_callback, view_port);
    view_port_input_callback_set(view_port, app_input_callback, event_queue);

    // Register view port in GUI
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    InputEvent event;
    bool running = true;
    bool save = false;
    while(running) {
        if(furi_message_queue_get(event_queue, &event, 150) == FuriStatusOk) {
            if((event.type == InputTypePress) || (event.type == InputTypeRepeat)) {
                switch(event.key) {
                case InputKeyLeft:
                    break;
                case InputKeyRight: {
                    // Save data if in capture mode
                    if(type == m_capture && pause == 1) {
                        running = false;
                        save = true;
                    }
                } break;
                case InputKeyUp:
                    break;
                case InputKeyDown:
                    break;
                case InputKeyOk:
                    pause ^= 1;
                    break;
                default:
                    running = false;
                    break;
                }
            }
        }
        view_port_update(view_port);
    }

    furi_hal_bus_disable(FuriHalBusTIM2);

    // Disable ADC interrupt and timer
    LL_ADC_DisableIT_OVR(ADC1);
    LL_TIM_DisableCounter(TIM2);

    // Stop DMA and switch back to original vector table
    LL_DMA_DisableChannel(DMA1, LL_DMA_CHANNEL_1);

    __disable_irq();
    SCB->VTOR = 0;
    __enable_irq();

    if(!save) {
        view_port_enabled_set(view_port, false);
        gui_remove_view_port(gui, view_port);
        view_port_free(view_port);

        free_all();

        // Switch back to original scene
        furi_record_close(RECORD_GUI);
        scene_manager_previous_scene(app->scene_manager);
        submenu_set_selected_item(app->submenu, 0);
    } else {
        view_port_enabled_set(view_port, false);
        gui_remove_view_port(gui, view_port);
        view_port_free(view_port);

        app->data = malloc(sizeof(uint16_t) * adc_buffer);
        memcpy(app->data, (uint16_t*)mvoltDisplay, sizeof(uint16_t) * adc_buffer);
        free_all();
        scene_manager_next_scene(app->scene_manager, ScopeSceneSave);
    }
}

bool scope_scene_run_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void scope_scene_run_on_exit(void* context) {
    ScopeApp* app = context;
    // Clear views
    widget_reset(app->widget);
}
