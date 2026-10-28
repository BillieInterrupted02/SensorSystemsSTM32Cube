/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include "PMDB16_LCD.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
// Stati Tx IR
typedef enum {
    TX_STATE_IDLE  = 0,
    TX_STATE_START = 1,
    TX_STATE_DATA  = 2,
    TX_STATE_STOP  = 3
} TxState_t;

// Keyboard
typedef struct {
	char character;
	uint8_t status;
	uint8_t duration;
	uint8_t sent;
} button;

typedef struct {
    GPIO_TypeDef* port;
    uint16_t pin;
} PinMap;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
// Keyboard Rows/Columns
#define KEY_C0 GPIOC,GPIO_PIN_11
#define KEY_C1 GPIOC,GPIO_PIN_10
#define KEY_C2 GPIOC,GPIO_PIN_9
#define KEY_C3 GPIOC,GPIO_PIN_8
#define KEY_R0 GPIOC,GPIO_PIN_3
#define KEY_R1 GPIOC,GPIO_PIN_2
#define KEY_R2 GPIOC,GPIO_PIN_13
#define KEY_R3 GPIOC,GPIO_PIN_12

// Keyboard Dimension
#define DIM 4

// Keyboard Buttons
#define DEBOUNCE 3
#define MAX_DURATION 250
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
TIM_HandleTypeDef htim2;
TIM_HandleTypeDef htim3;
TIM_HandleTypeDef htim4;

UART_HandleTypeDef huart1;

/* USER CODE BEGIN PV */
// KEYBOARD
PinMap columns[] = {
    { KEY_C3 },
    { KEY_C2 },
    { KEY_C1 },
    { KEY_C0 }
};

PinMap rows[] = {
    { KEY_R0 },
    { KEY_R1 },
    { KEY_R2 },
    { KEY_R3 }
};

volatile button keyboard[DIM][DIM] = {
    { {'C', 1, 0, 0}, {'D', 1, 0, 0}, {'E', 1, 0, 0}, {'F', 1, 0, 0} },
    { {'8', 1, 0, 0}, {'9', 1, 0, 0}, {'A', 1, 0, 0}, {'B', 1, 0, 0} },
    { {'4', 1, 0, 0}, {'5', 1, 0, 0}, {'6', 1, 0, 0}, {'7', 1, 0, 0} },
    { {'0', 1, 0, 0}, {'1', 1, 0, 0}, {'2', 1, 0, 0}, {'3', 1, 0, 0} }
};

uint8_t active_col = 0;



// TX IR
volatile uint8_t  tx_data_buffer = 'G';
volatile uint8_t  bit_counter = 0;
volatile TxState_t tx_state = TX_STATE_IDLE;

// RX IR
uint8_t rx_data_buffer; // Byte ricevuto dall'IR

// LCD
volatile uint8_t ir_received_char = 0;
volatile uint8_t ir_data_ready = 0;
char lcd_buffer[17]; // Buffer per la stringa (16 char + terminatore)
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_TIM2_Init(void);
static void MX_TIM3_Init(void);
static void MX_TIM4_Init(void);
static void MX_USART1_UART_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

// Avvia l'invio di un singolo byte tramite IR UART
void IR_UART_SendByte(uint8_t data)
{
    // Solo se il canale TX è libero posso iniziare nuova trasmissione(IDLE)
    if (tx_state == TX_STATE_IDLE)
    {
        tx_data_buffer = data;
        tx_state = TX_STATE_START;      // Ci prepariamo a inviare lo Start Bit

        __HAL_TIM_SET_COUNTER(&htim3, 0); // Azzera CNT di TIM3 affinchè lo Start Bit duri un bit time pieno
        // e non di meno perchè magari CNT era ad un altro valore. CNT arriva a ARR (99) prima di andare in
        // overflow e scattare l'ISR, se dalla scorsa ricezione CNT era ad un altro valore non conta fino a
        // 100 ma di meno
        HAL_TIM_Base_Start_IT(&htim3); // TIM3 (2400 Hz) scandisce i bit nella Trasmissione IR
    }
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM3) // 2400 Hz, scandisce i bit IR Tx (UART1 Rx ha baud Rate = 2400 b/s)
    {
        switch (tx_state)
        {
            case TX_STATE_IDLE:
            	// Non dobbiamo inviare nulla, fermiamo il LED IR
                HAL_TIM_PWM_Stop(&htim2, TIM_CHANNEL_3);
                break;

            case TX_STATE_START:
            	// Inviamo lo Start Bit '0' accendendo il LD IR
            	// Bit = 0 -> LD IR ON -> RX = LOW (START UART)
                HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_3); // IR LED ON

                // Ora passiamo ai dati
                tx_state = TX_STATE_DATA; // Passa alla gestione dati
                bit_counter = 0;          // Resetta contatore bit
                // Nel prossimo caso ci torno tanto nel prossimo bit rate
                break;

            case TX_STATE_DATA:
            	// Inviamo 8 bit
            	// Bit Masking
            	// Esempio '1': 0(1)00 0001  >> 6  =  0000 000(1)
            	// 0000 000(1) & 0000 0001 = 0000 0001 ON
            	// Esempio '1': (0)100 0001  >> 7  =  0000 000(0)
				// 0000 000(0) & 0000 0001 = 0000 0000 OFF

                if (((tx_data_buffer >> bit_counter) & 0x01) == 0)
                {
                    HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_3); // Bit '0' -> ON
                }
                else
                {
                    HAL_TIM_PWM_Stop(&htim2, TIM_CHANNEL_3);  // Bit '1' -> OFF
                }

                bit_counter++; // Avanziamo nel buffer

                if (bit_counter >= 8) // Quando abbiamo finito
                {
                    tx_state = TX_STATE_STOP;
                }
                break;

            case TX_STATE_STOP: // Stop Bit ('1')
                HAL_TIM_PWM_Stop(&htim2, TIM_CHANNEL_3); // IR LED OFF

                // Ritorniamo in IDLE
                tx_state = TX_STATE_IDLE;
                // Finchè non premiamo altri tasti non abbiamo bisogno della Callback a 2400Hz
                HAL_TIM_Base_Stop_IT(&htim3);
                break;
        }
    }

    if (htim->Instance == TIM4) { // 500 Hz (scansione colonne KEYBOARD)
    	        for(int r=0; r<DIM; r++){
    	        	// Se il tasto è premuto
    	            if(HAL_GPIO_ReadPin(rows[r].port, rows[r].pin) == GPIO_PIN_RESET){
    	            	// Se è premuto da poco
    	                if(keyboard[r][active_col].duration < MAX_DURATION ) { // MAX_DURATION = 250*2ms = 0.5s
    	                    keyboard[r][active_col].duration++; // Aggiorniamo
    	                }
    	            } else { // Se il tasto non è premuto azzeriamo tutto
    	                keyboard[r][active_col].duration = 0;
    	                keyboard[r][active_col].sent = 0;
    	            }
    	        }
    	        // Deselezioniamo la colonna appena letta
    	        HAL_GPIO_WritePin(columns[active_col].port, columns[active_col].pin, GPIO_PIN_RESET);

    	        // Aggiorniamo la colonna da attivare
    	        active_col=(active_col+1) % DIM;

    	        // Attiviamo la nuova colonna
    	        HAL_GPIO_WritePin(columns[active_col].port, columns[active_col].pin, GPIO_PIN_SET);
	}
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1)
    {
    	ir_received_char = rx_data_buffer;
    	ir_data_ready = 1; // Siamo pronti a scrivere sull'LCD
        HAL_UART_Receive_IT(&huart1, &rx_data_buffer, 1); // ci rimettiamo in ascolto
    }

}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_TIM2_Init();
  MX_TIM3_Init();
  MX_TIM4_Init();
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */

  // LCD initialization
  lcd_initialize();
  lcd_backlight_ON();

  // Prepara alla ricezione la UART1
  HAL_UART_Receive_IT(&huart1, &rx_data_buffer, 1);

  // Prepara la scansione della keyboard
  HAL_TIM_Base_Start_IT(&htim4);
  HAL_GPIO_WritePin(columns[active_col].port, columns[active_col].pin, GPIO_PIN_SET);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
	  // --- KEYBOARD SCANNING ---
	  	  for(int j=0;j<DIM;j++){
	  	  		  for(int i=0;i<DIM;i++){
	  	  			  if((keyboard[i][j].duration>=DEBOUNCE)&&(keyboard[i][j].sent==0) ){
	  	  				  IR_UART_SendByte(keyboard[i][j].character);
	  	  				  keyboard[i][j].sent=1;
	  	  			  }
	  	  		  }
	  	  }


	  	  // --- PRINT LCD  ---
			if (ir_data_ready == 1) {

				// Formatta la stringa col dato ricevuto
				sprintf(lcd_buffer, "Character: %c", ir_received_char);

				// Stampa
				lcd_println(lcd_buffer, 0);

				// Resetta la flag
				ir_data_ready = 0;
			}
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 16;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
  RCC_OscInitStruct.PLL.PLLQ = 7;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 220;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 9;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 5;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_PWM_ConfigChannel(&htim2, &sConfigOC, TIM_CHANNEL_3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */
  HAL_TIM_MspPostInit(&htim2);

}

/**
  * @brief TIM3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM3_Init(void)
{

  /* USER CODE BEGIN TIM3_Init 0 */

  /* USER CODE END TIM3_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM3_Init 1 */

  /* USER CODE END TIM3_Init 1 */
  htim3.Instance = TIM3;
  htim3.Init.Prescaler = 349;
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = 99;
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim3, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM3_Init 2 */

  /* USER CODE END TIM3_Init 2 */

}

/**
  * @brief TIM4 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM4_Init(void)
{

  /* USER CODE BEGIN TIM4_Init 0 */

  /* USER CODE END TIM4_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM4_Init 1 */

  /* USER CODE END TIM4_Init 1 */
  htim4.Instance = TIM4;
  htim4.Init.Prescaler = 8399;
  htim4.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim4.Init.Period = 19;
  htim4.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim4.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim4) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim4, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim4, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM4_Init 2 */

  /* USER CODE END TIM4_Init 2 */

}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 2400;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1|GPIO_PIN_2|GPIO_PIN_12|GPIO_PIN_13
                          |GPIO_PIN_14|GPIO_PIN_15, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_8|GPIO_PIN_9|GPIO_PIN_10|GPIO_PIN_11, GPIO_PIN_RESET);

  /*Configure GPIO pins : PC13 PC2 PC3 PC12 */
  GPIO_InitStruct.Pin = GPIO_PIN_13|GPIO_PIN_2|GPIO_PIN_3|GPIO_PIN_12;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pin : PA4 */
  GPIO_InitStruct.Pin = GPIO_PIN_4;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pins : PB1 PB2 PB12 PB13
                           PB14 PB15 */
  GPIO_InitStruct.Pin = GPIO_PIN_1|GPIO_PIN_2|GPIO_PIN_12|GPIO_PIN_13
                          |GPIO_PIN_14|GPIO_PIN_15;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pins : PC8 PC9 PC10 PC11 */
  GPIO_InitStruct.Pin = GPIO_PIN_8|GPIO_PIN_9|GPIO_PIN_10|GPIO_PIN_11;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
