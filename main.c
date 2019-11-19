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
* (c) (2019), Cypress Semiconductor Corporation. All rights reserved.
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

#include "cy_pdl.h"
#include "cyhal.h"
#include "cybsp.h"
#include "ak4954a.h"

/*******************************************************************************
* Macros
********************************************************************************/
/* Size of the recorded buffer */
#define BUFFER_SIZE     32768u

/* Number of channels (Stereo) */
#define NUM_CHANNELS    2u

/* Master I2C Timeout */
#define MI2C_TIMEOUT    10u         /* in ms */

/* Master Clock (MCLK) Frequency for the audio codec */
#define MCLK_FREQ       2042000   /* in Hz */

/* Duty cycle for the MCLK PWM */
#define MCLK_DUTY_CYCLE 50.0f       /* in %  */

/* DMA Maximum loop transfer size */
#define DMA_LOOP_SIZE   256u

/*******************************************************************************
* Function Prototypes
********************************************************************************/
cy_rslt_t mi2c_transmit(uint8_t reg_adrr, uint8_t data);

/*******************************************************************************
* Global Variables
********************************************************************************/
/* Array containing the recorded data (stereo) */
int16_t recorded_data[NUM_CHANNELS][BUFFER_SIZE];

/* Number of elements recorded by the PDM/PCM */
uint32_t pdm_count = 0;

/* Master I2C variables */
cyhal_i2c_t mi2c;

const cyhal_i2c_cfg_t mi2c_cfg = {
    .is_slave        = false,
    .address         = 0,
    .frequencyhal_hz = 400000
};

/* Master Clock PWM */
cyhal_pwm_t mclk_pwm;


/*******************************************************************************
* Function Name: main
********************************************************************************
* Summary:
* This is the main function for CM4 CPU. It does...
*    1. Initialize the hardware and configure the audio codec.
*    2. Set up the DMAs.
*    3. Enable the audio subsystem.
*    Do Forever loop:
*    4. Go to sleep.
*    5. Check if the user button is pressed or not.
*    6. If pressed, start recording.
*    7. If not pressed, start playing a record.
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

    /* Initialize the device and board peripherals */
    result = cybsp_init() ;
    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(0);
    }

    /* Enable global interrupts */
    __enable_irq();

    /* Initialize the User Button */
    cyhal_gpio_init((cyhal_gpio_t) CYBSP_USER_BTN, CYHAL_GPIO_DIR_INPUT, CYHAL_GPIO_DRIVE_PULLUP, CYBSP_BTN_OFF);
    cyhal_gpio_enable_event((cyhal_gpio_t) CYBSP_USER_BTN, CYHAL_GPIO_IRQ_BOTH, CYHAL_ISR_PRIORITY_DEFAULT, true);

    /* Initialize the Master Clock with a PWM */
    cyhal_pwm_init(&mclk_pwm, (cyhal_gpio_t) P5_0, NULL);
    cyhal_pwm_set_duty_cycle(&mclk_pwm, MCLK_DUTY_CYCLE, MCLK_FREQ);
    cyhal_pwm_start(&mclk_pwm);

    /* Wait for a bit the MCLK to clock the audio codec */
    Cy_SysLib_Delay(1);

    /* Initialize the I2C Master */
    cyhal_i2c_init(&mi2c, CYBSP_I2C_SDA, CYBSP_I2C_SCL, NULL);
    cyhal_i2c_configure(&mi2c, &mi2c_cfg);

    /* Initialize the I2S block */
    Cy_I2S_Init(CYBSP_I2S_HW, &CYBSP_I2S_config);
    Cy_I2S_ClearTxFifo(CYBSP_I2S_HW);
    /* Put at least one frame into the Tx FIFO */
    Cy_I2S_WriteTxData(CYBSP_I2S_HW, 0UL);
    Cy_I2S_WriteTxData(CYBSP_I2S_HW, 0UL);   
  
    /* Initialize PDM/PCM block */
    Cy_PDM_PCM_Init(CYBSP_PDM_HW, &CYBSP_PDM_config);

    /* Configure the AK494A codec and enable it */
    ak4954A_init(mi2c_transmit);
    ak4954A_activate();

    /* Initialize the DMAs and their descriptor addresses */
    Cy_DMA_Descriptor_Init(&CYBSP_DMA_PDM_Descriptor_0, &CYBSP_DMA_PDM_Descriptor_0_config);
    Cy_DMA_Descriptor_SetYloopDataCount(&CYBSP_DMA_PDM_Descriptor_0, BUFFER_SIZE*NUM_CHANNELS/DMA_LOOP_SIZE); 
    Cy_DMA_Descriptor_SetSrcAddress(&CYBSP_DMA_PDM_Descriptor_0, (void *) &CYBSP_PDM_HW->RX_FIFO_RD);
    Cy_DMA_Descriptor_SetDstAddress(&CYBSP_DMA_PDM_Descriptor_0, (void *) &recorded_data[0]);
    Cy_DMA_Channel_Init(CYBSP_DMA_PDM_HW, CYBSP_DMA_PDM_CHANNEL, &CYBSP_DMA_PDM_channelConfig);
    Cy_DMA_Enable(CYBSP_DMA_PDM_HW);

    Cy_DMA_Descriptor_Init(&CYBSP_DMA_I2S_Descriptor_0, &CYBSP_DMA_I2S_Descriptor_0_config);
    Cy_DMA_Descriptor_SetYloopDataCount(&CYBSP_DMA_I2S_Descriptor_0, BUFFER_SIZE*NUM_CHANNELS/DMA_LOOP_SIZE); 
    Cy_DMA_Descriptor_SetSrcAddress(&CYBSP_DMA_I2S_Descriptor_0, (void *) &recorded_data[0]);
    Cy_DMA_Descriptor_SetDstAddress(&CYBSP_DMA_I2S_Descriptor_0, (void *) &CYBSP_I2S_HW->TX_FIFO_WR); 
    Cy_DMA_Channel_Init(CYBSP_DMA_I2S_HW, CYBSP_DMA_I2S_CHANNEL, &CYBSP_DMA_I2S_channelConfig);
    Cy_DMA_Enable(CYBSP_DMA_I2S_HW);  
   
    /* Enable Audio Subsystem */
    Cy_I2S_EnableTx(CYBSP_I2S_HW);
    Cy_PDM_PCM_Enable(CYBSP_PDM_HW);

    for(;;)
    {
        cyhal_system_sleep();

        /* Check if the button is pressed */
        if (cyhal_gpio_read(CYBSP_USER_BTN) == CYBSP_BTN_PRESSED)
        {
            /* Enable DMA to record from the microphone */
            Cy_DMA_Channel_Enable(CYBSP_DMA_PDM_HW, CYBSP_DMA_PDM_CHANNEL);
        }
        else
        {
            /* If not pressed, stop recording */
            Cy_DMA_Channel_Disable(CYBSP_DMA_PDM_HW, CYBSP_DMA_PDM_CHANNEL);

            /* Extract the number of elements recorded */
            pdm_count = CYBSP_DMA_PDM_HW->CH_STRUCT[CYBSP_DMA_PDM_CHANNEL].CH_IDX;            

            /* Reset the channel index for the next recording */
            CYBSP_DMA_PDM_HW->CH_STRUCT[CYBSP_DMA_PDM_CHANNEL].CH_IDX = 0;

            /* If pdm_count = 0, it means the maximum number of bytes were transferred */
            if (pdm_count == 0)
            {
                /* Set to the buffer size */
                pdm_count = BUFFER_SIZE*NUM_CHANNELS;
            }

            /* Set up the DMAs to play the recorded data */
            Cy_DMA_Descriptor_SetYloopDataCount(&CYBSP_DMA_I2S_Descriptor_0, pdm_count/DMA_LOOP_SIZE);

            /* Start playing the recorded data by enabling the I2S DMA */
            Cy_DMA_Channel_Enable(CYBSP_DMA_I2S_HW, CYBSP_DMA_I2S_CHANNEL); 
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
    result = cyhal_i2c_master_write(&mi2c, AK4954A_I2C_ADDR, buffer, 2, MI2C_TIMEOUT, true);

    return result;
}


/* [] END OF FILE */