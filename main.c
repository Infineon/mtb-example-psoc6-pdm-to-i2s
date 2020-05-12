/******************************************************************************
* File Name:   main.c
*
* Description: This is the source code for the PDM to I2S Example
*              for ModusToolbox.
*
* Related Document: See Readme.md
*
*
*******************************************************************************
* (c) (2019-20), Cypress Semiconductor Corporation. All rights reserved.
*******************************************************************************
* This software, including source code, documentation and related materials
* ("Software"), is owned by Cypress Semiconductor Corporation or one of its
* subsidiaries ("Cypress") and is protected by and subject to worldwide patent
* protection (United States and foreign), United States copyright laws and
* international treaty provisions. Therefore, you may use this Software only
* as provided in the license agreement accompanying the software package from
* which you obtained this Software ("EULA").
*
* If no EULA applies, Cypress hereby grants you a personal, non-exclusive,
* non-transferable license to copy, modify, and compile the Software source
* code solely for use in connection with Cypress's integrated circuit products.
* Any reproduction, modification, translation, compilation, or representation
* of this Software except as specified above is prohibited without the express
* written permission of Cypress.
*
* Disclaimer: THIS SOFTWARE IS PROVIDED AS-IS, WITH NO WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, NONINFRINGEMENT, IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. Cypress
* reserves the right to make changes to the Software without notice. Cypress
* does not assume any liability arising out of the application or use of the
* Software or any product or circuit described in the Software. Cypress does
* not authorize its products for use in any products where a malfunction or
* failure of the Cypress product may reasonably be expected to result in
* significant property damage, injury or death ("High Risk Product"). By
* including Cypress's product in a High Risk Product, the manufacturer of such
* system or application assumes all risk of such use and in doing so agrees to
* indemnify Cypress against all liability.
*******************************************************************************/

#include "cyhal.h"
#include "cybsp.h"
#include "ak4954a.h"

/*******************************************************************************
* Macros
********************************************************************************/
/* Number of channels (Stereo) */
#define NUM_CHANNELS        2u

/* Size of the recorded buffer */
#define BUFFER_SIZE         32768u

/* PDM Half FIFO Size */
#define PDM_HALF_FIFO_SIZE  128u

/* Master I2C Timeout */
#define MI2C_TIMEOUT_MS     10u        /* in ms */

/* Master Clock (MCLK) Frequency for the audio codec */
#define MCLK_FREQ_HZ        2048000    /* in Hz */

/* Duty cycle for the MCLK PWM */
#define MCLK_DUTY_CYCLE     50.0f       /* in %  */

/* Desired Sample Rate */
#define SAMPLE_RATE_HZ      8000u

/* Decimation Rate of the PDM/PCM block */
#define DECIMATION_RATE     64u

/* Clock Settings */
#define AUDIO_SYS_CLOCK_HZ  98304000u   /* in Hz */

/* PWM MCLK Pin */
#define MCLK_PIN            P5_0

/* PDM/PCM Pins */
#define PDM_DATA            P10_5
#define PDM_CLK             P10_4

/*******************************************************************************
* Function Prototypes
********************************************************************************/
cy_rslt_t mi2c_transmit(uint8_t reg_adrr, uint8_t data);
void i2s_isr_handler(void *arg, cyhal_i2s_event_t event);
void clock_init(void);

/*******************************************************************************
* Global Variables
********************************************************************************/
/* Array containing the recorded data (stereo) */
int16_t recorded_data[NUM_CHANNELS][BUFFER_SIZE];

/* I2S completion flag */
volatile bool i2s_flag = false;

/* HAL Objects */
cyhal_pwm_t mclk_pwm;
cyhal_i2c_t mi2c;
cyhal_i2s_t i2s;
cyhal_pdm_pcm_t pdm_pcm;
cyhal_clock_t audio_clock;
cyhal_clock_t pll_clock;
cyhal_clock_t fll_clock;
cyhal_clock_t system_clock;

/* HAL Configs */
const cyhal_i2c_cfg_t mi2c_cfg = {
    .is_slave        = false,
    .address         = 0,
    .frequencyhal_hz = 400000
};

const cyhal_i2s_pins_t i2s_pins = {
    .sck  = P5_1,
    .ws   = P5_2,
    .data = P5_3,
};

const cyhal_i2s_config_t i2s_config = {
    .is_tx_slave    = false,            /* TX is Master */
    .is_rx_slave    = false,            /* RX not used */
    .mclk_hz        = 0,                /* External MCLK not used */
    .channel_length = 32,               /* In bits */
    .word_length    = 16,               /* In bits */
    .sample_rate_hz = SAMPLE_RATE_HZ,   /* In Hz */
};

const cyhal_pdm_pcm_cfg_t pdm_pcm_cfg = 
{
    .sample_rate     = SAMPLE_RATE_HZ,
    .decimation_rate = DECIMATION_RATE,
    .mode            = CYHAL_PDM_PCM_MODE_STEREO, 
    .word_length     = 16,  /* bits */
    .left_gain       = 0,   /* dB */
    .right_gain      = 0,   /* dB */
};

/*******************************************************************************
* Function Name: main
********************************************************************************
* Summary:
* This is the main function for CM4 CPU. It does:
*    1. Initialize the hardware and configure the audio codec.
*    2. Enable the audio subsystem.
*    Do Forever loop:
*    3. Go to sleep.
*    4. Check if the user button is pressed or not.
*    5. If pressed, start recording.
*    6. If not pressed, start playing a record.
*
* Parameters:
*  void
*
* Return:
*  int
*
*******************************************************************************/
int main(void)
{
    cy_rslt_t result;
    int16_t *audio_data_ptr = NULL;
    uint32_t diff;
    bool     is_recording = false;
    bool     is_playing = false;

    /* Initialize the device and board peripherals */
    result = cybsp_init() ;
    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(0);
    }

    /* Enable global interrupts */
    __enable_irq();

    /* Init the clocks */
    clock_init();

    /* Initialize the User LED */
    cyhal_gpio_init(CYBSP_USER_LED, CYHAL_GPIO_DIR_OUTPUT, CYHAL_GPIO_DRIVE_STRONG, CYBSP_LED_STATE_OFF);

    /* Initialize the User Button */
    cyhal_gpio_init(CYBSP_USER_BTN, CYHAL_GPIO_DIR_INPUT, CYHAL_GPIO_DRIVE_PULLUP, CYBSP_BTN_OFF);
    cyhal_gpio_enable_event(CYBSP_USER_BTN, CYHAL_GPIO_IRQ_BOTH, CYHAL_ISR_PRIORITY_DEFAULT, true);

    /* Initialize the Master Clock with a PWM */
    cyhal_pwm_init(&mclk_pwm, MCLK_PIN, NULL);
    cyhal_pwm_set_duty_cycle(&mclk_pwm, MCLK_DUTY_CYCLE, MCLK_FREQ_HZ);
    cyhal_pwm_start(&mclk_pwm);

    /* Wait for the MCLK to clock the audio codec */
    cyhal_system_delay_ms(AK4954A_RESET_WAIT_DELAY_MS);

    /* Initialize the I2C Master */
    cyhal_i2c_init(&mi2c, CYBSP_I2C_SDA, CYBSP_I2C_SCL, NULL);
    cyhal_i2c_configure(&mi2c, &mi2c_cfg);

    /* Initialize the I2S */
    cyhal_i2s_init(&i2s, &i2s_pins, NULL, NC, &i2s_config, &audio_clock);
    cyhal_i2s_register_callback(&i2s, i2s_isr_handler, NULL);
    cyhal_i2s_enable_event(&i2s, CYHAL_I2S_ASYNC_TX_COMPLETE, CYHAL_ISR_PRIORITY_DEFAULT, true);
  
    /* Initialize the PDM/PCM block */
    cyhal_pdm_pcm_init(&pdm_pcm, PDM_DATA, PDM_CLK, &audio_clock, &pdm_pcm_cfg);
    cyhal_pdm_pcm_enable_event(&pdm_pcm, CYHAL_PDM_PCM_ASYNC_COMPLETE, CYHAL_ISR_PRIORITY_DEFAULT, true);
    
    /* Configure the AK494A codec and enable it */
    result = ak4954a_init(mi2c_transmit);
    /* If the initialization fails, reset the device */
    if (result != 0)
    {
        NVIC_SystemReset();
    }
    ak4954a_activate();
    ak4954a_adjust_volume(AK4954A_HP_DEFAULT_VOLUME);
   
    for(;;)
    {
        cyhal_system_sleep();

        /* Check if the button is pressed */
        if (cyhal_gpio_read(CYBSP_USER_BTN) == CYBSP_BTN_PRESSED)
        {
            /* Check if this is a new recording */
            if (audio_data_ptr == NULL)
            {
                /* Enable the PDM/PCM to capture audio data */
                cyhal_pdm_pcm_start(&pdm_pcm);

                /* Reset the audio data pointer */
                audio_data_ptr = (int16_t *) recorded_data;

                /* Set to be recording */
                is_recording = true;
            }
            else
            {
                /* Check if still recording */
                if (is_recording)
                {
                    /* Read recorded data */
                    cyhal_pdm_pcm_read_async(&pdm_pcm, (void *) audio_data_ptr, PDM_HALF_FIFO_SIZE);

                    /* Update pointer to the recorded data and PDM counter */
                    audio_data_ptr += PDM_HALF_FIFO_SIZE;
                    
                    /* Check if pointer reached the limit */
                    diff = (uint32_t) audio_data_ptr - (uint32_t) recorded_data;
                    if (diff > ((BUFFER_SIZE * NUM_CHANNELS) - PDM_HALF_FIFO_SIZE))
                    {
                        is_recording = false;
                        cyhal_pdm_pcm_stop(&pdm_pcm);
                    }
                }
            }           
        }
        else
        {
            /* If recording, stop it */
            if (is_recording)
            {
                is_recording = false;
                cyhal_pdm_pcm_stop(&pdm_pcm);
            }

            /* Check if not playing */
            if (!is_playing)
            {
                /* Check if any data is recorded */
                if (audio_data_ptr != NULL)
                {
                    /* Start a new playing */
                    is_playing = true;

                    /* Check how many samples in the buffer */
                    diff = ((uint32_t) audio_data_ptr - (uint32_t) recorded_data)/2;

                    /* Start playing */
                    cyhal_i2s_start_tx(&i2s);

                    /* Skip the first samples to avoid some noise during
                     * the PDM transition from idle to sampling */
                    audio_data_ptr = (int16_t*) recorded_data + PDM_HALF_FIFO_SIZE;
                    cyhal_i2s_write_async(&i2s, audio_data_ptr, diff);

                    /* Turn on the User LED */
                    cyhal_gpio_write(CYBSP_USER_LED, CYBSP_LED_STATE_ON);
                }
            }

            /* Check if completed playing */
            if (i2s_flag)
            {
                /* Clear multiple variables */
                i2s_flag = false;
                audio_data_ptr = NULL;
                is_playing = false;
            } 
        }
    }
}

/*******************************************************************************
* Function Name: mi2c_transmit
********************************************************************************
* Summary:
*  I2C Master function to transmit data to the given address.
*
* Parameters:
*  reg_addr: address to be updated
*  data: 8-bit data to be written in the register
*
* Return:
*  cy_rslt_t - I2C master transaction error status. 
*              Returns CY_RSLT_SUCCESS if succeeded.
*
*******************************************************************************/
cy_rslt_t mi2c_transmit(uint8_t reg_addr, uint8_t data)
{
    cy_rslt_t result;
    uint8_t buffer[AK4954A_PACKET_SIZE];
    
    buffer[0] = reg_addr;
    buffer[1] = data;

    /* Send the data over the I2C */
    result = cyhal_i2c_master_write(&mi2c, AK4954A_I2C_ADDR, buffer, 2, MI2C_TIMEOUT_MS, true);

    return result;
}

/*******************************************************************************
* Function Name: i2s_isr_handler
********************************************************************************
* Summary:
*  I2S ISR handler. Stop the I2S TX and turn OFF the User LED.
*
* Parameters:
*  arg: not used
*  event: event that occurred
*
*******************************************************************************/
void i2s_isr_handler(void *arg, cyhal_i2s_event_t event)
{
    (void) arg;
    (void) event;

    /* Set flag */
    i2s_flag = true;

    /* Stop the I2S TX */
    cyhal_i2s_stop_tx(&i2s);

    /* Turn off the User LED */
    cyhal_gpio_write(CYBSP_USER_LED, CYBSP_LED_STATE_OFF);
}

/*******************************************************************************
* Function Name: clock_init
********************************************************************************
* Summary:
*  Initialize the clocks in the system.
*
*******************************************************************************/
void clock_init(void)
{
    /* Initialize the PLL */
    cyhal_clock_get(&pll_clock, &CYHAL_CLOCK_PLL[0]);
    cyhal_clock_init(&pll_clock);
    cyhal_clock_set_frequency(&pll_clock, AUDIO_SYS_CLOCK_HZ, NULL);

    /* Initialize the audio subsystem clock (HFCLK1) */
    cyhal_clock_get(&audio_clock, &CYHAL_CLOCK_HF[1]);
    cyhal_clock_init(&audio_clock);
    cyhal_clock_set_source(&audio_clock, &pll_clock);

    /* Drop HFCK1 frequency for power savings */
    cyhal_clock_set_divider(&audio_clock, 4);	
    cyhal_clock_set_enabled(&audio_clock, true, true);

    /* Initialize the system clock (HFCLK0) */
    cyhal_clock_get(&system_clock, &CYHAL_CLOCK_HF[0]);
    cyhal_clock_init(&system_clock);
    cyhal_clock_set_source(&system_clock, &pll_clock);

    /* Disable the FLL for power savings */
    cyhal_clock_get(&fll_clock, &CYHAL_CLOCK_FLL);
    cyhal_clock_init(&fll_clock);
    cyhal_clock_set_enabled(&fll_clock, false, true);
}


/* [] END OF FILE */
