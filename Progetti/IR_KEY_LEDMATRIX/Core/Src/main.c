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
#include "stdio.h"
#include "string.h"

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
	uint8_t duration;
	uint8_t sent;
	uint8_t idx;
} button;

typedef struct {
    GPIO_TypeDef* port;
    uint16_t pin;
} PinMap;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
// KEYBOARD
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

// LED MATRIX
#define NUM_COLS_LED 5
#define NUM_SYMBOLS 16
#define RCLK_PORT GPIOB
#define RCLK_PIN  GPIO_PIN_6

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
SPI_HandleTypeDef hspi1;
DMA_HandleTypeDef hdma_spi1_tx;

TIM_HandleTypeDef htim2;
TIM_HandleTypeDef htim3;
TIM_HandleTypeDef htim4;
TIM_HandleTypeDef htim5;

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
    { {'C', 0, 0, 12}, {'D', 0, 0, 13}, {'E', 0, 0, 14}, {'F', 0, 0, 15} },
    { {'8', 0, 0, 8}, {'9', 0, 0, 9}, {'A', 0, 0, 10}, {'B', 0, 0, 11} },
    { {'4', 0, 0, 4}, {'5', 0, 0, 5}, {'6', 0, 0, 6}, {'7', 0, 0, 7} },
    { {'0', 0, 0, 0}, {'1', 0, 0, 1}, {'2', 0, 0, 2}, {'3', 0, 0, 3} }
};

uint8_t active_col = 0;



// TX IR
volatile uint8_t  tx_data_buffer = 'G';
volatile uint8_t  bit_counter = 0;
volatile TxState_t tx_state = TX_STATE_IDLE;

// RX IR
uint8_t rx_data_buffer; // Byte ricevuto dall'IR
volatile uint8_t ir_received_char = 0;
volatile uint8_t ir_data_ready = 0;

// LED MATRIX
static const uint8_t COL_LED[NUM_COLS_LED] = {16, 8, 4, 2, 1};
static const uint8_t SYMBOLS[NUM_SYMBOLS+1][NUM_COLS_LED] = {
    { 62,  69,  73,  81,  62}, // '0'
    {  0,  33, 127,   1,   0}, // '1'
    { 33,  67,  69,  73,  49}, // '2'
    { 34,  65,  73,  73,  54}, // '3'
    { 12,  20,  36, 127,   4}, // '4'
    {114,  81,  81,  81,  78}, // '5'
    { 30,  41,  73,  73,   6}, // '6'
    { 64,  71,  72,  80,  96}, // '7'
    { 54,  73,  73,  73,  54}, // '8'
    { 48,  73,  73,  74,  60}, // '9'
    { 63,  72,  72,  72,  63}, // 'A'
    {127,  73,  73,  73,  54}, // 'B'
    { 62,  65,  65,  65,  34}, // 'C'
    {127,  65,  65,  65,  62}, // 'D'
    {127,  73,  73,  73,  65}, // 'E'
    {127,  72,  72,  72,  64}, // 'F'
    {  0,   0,   0,   0,   0}  // spento
};
static volatile uint8_t sym_idx = NUM_SYMBOLS;   // cosa mostra la matrice (spenta se NUM_SYMBOLS)
static uint8_t col_cnt = 0;                // colonna della matrice in refresh

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_TIM2_Init(void);
static void MX_TIM3_Init(void);
static void MX_TIM4_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_SPI1_Init(void);
static void MX_TIM5_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static inline void spi_send(uint8_t col, uint8_t row){
	static uint8_t buffer[2]; // 2 byte
	// DAISY RING
	buffer[0]=row; // prima la riga
	buffer[1]=col; // poi la colonna

	// Ogni 4 ms trasmettiamo (refreshamo)
	HAL_SPI_Transmit_DMA(&hspi1, buffer, 2);
}

void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi){
	if(hspi->Instance==SPI1){
		// Una volta terminata la comunicazione dei 2 byte
		// diamo un impulso su RCLK per far scattare il latch
		// dei due shift register
		HAL_GPIO_WritePin(RCLK_PORT, RCLK_PIN, GPIO_PIN_SET);
		HAL_GPIO_WritePin(RCLK_PORT, RCLK_PIN, GPIO_PIN_RESET);
	}
}


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
    if (htim->Instance == TIM3) // 2400 Hz, scandisce i bit da mandare (TX IR)
    {
        switch (tx_state)
        {
            case TX_STATE_IDLE:
            	// HIGH -> LED OFF
                HAL_TIM_PWM_Stop(&htim2, TIM_CHANNEL_3);
                break;

            case TX_STATE_START:
            	// START BIT = LOW -> LED ON per 1/2400 s
                HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_3); // IR LED ON
                tx_state = TX_STATE_DATA; // Passa alla gestione dati
                bit_counter = 0;          // Resetta contatore bit dati
                break;

            case TX_STATE_DATA:
            	// Inviamo 8 bit
            	// Bit Masking
            	// Esempio '1': 0(1)00 0001  >> 6  =  0000 000(1)
            	// 0000 000(1) & 0000 0001 = 0000 0001
            	// Esempio '1': (0)100 0001  >> 7  =  0000 000(0)
				// 0000 000(0) & 0000 0001 = 0000 0000

                if (((tx_data_buffer >> bit_counter) & 0x01) == 0)
                {
                    // 0 -> LED ON
                	HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_3);
                }
                else
                {
                    // 1 -> LED OFF
                	HAL_TIM_PWM_Stop(&htim2, TIM_CHANNEL_3);
                }

                bit_counter++; // Avanziamo nel buffer

                if (bit_counter >= 8) // Quando abbiamo finito
                {
                    tx_state = TX_STATE_STOP;
                }
                break;

            case TX_STATE_STOP:
            	// STOP BIT = HIGH -> LED OFF
                HAL_TIM_PWM_Stop(&htim2, TIM_CHANNEL_3);

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
    	                if(keyboard[r][active_col].duration < MAX_DURATION ) {
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


    if (htim->Instance == TIM5) { // 4ms, refresh colonne
    	spi_send(COL_LED[col_cnt], SYMBOLS[sym_idx][col_cnt]);
    	col_cnt=(col_cnt+1)%NUM_COLS_LED;
    }



}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1)
    {
    	ir_received_char = rx_data_buffer;
    	ir_data_ready = 1; // Siamo pronti a scrivere sulla matrice Led
        HAL_UART_Receive_IT(&huart1, &rx_data_buffer, 1); // ci rimettiamo in ascolto
    }

}

/* Logica ASCII nel while
 * static uint8_t char_to_idx(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return NUM_SYMBOLS;   // carattere sconosciuto -> matrice spenta
}
 */


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
  MX_DMA_Init();
  MX_TIM2_Init();
  MX_TIM3_Init();
  MX_TIM4_Init();
  MX_USART1_UART_Init();
  MX_SPI1_Init();
  MX_TIM5_Init();
  /* USER CODE BEGIN 2 */


  // Prepara alla ricezione la UART1
  HAL_UART_Receive_IT(&huart1, &rx_data_buffer, 1);

  // Prepara la scansione della keyboard
  HAL_TIM_Base_Start_IT(&htim4);
  HAL_GPIO_WritePin(columns[active_col].port, columns[active_col].pin, GPIO_PIN_SET);

  // Matrice Led
  HAL_TIM_Base_Start_IT(&htim5);

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

	  	  // MATRICE LED
	  	  if (ir_data_ready==1){
	  		  // Cerca l'idx del simbolo da mostrare

	  		  // Tramite i for
	  		  for (int j=0; j<DIM; j++){
	  			  for (int i=0; i<DIM; i++){
	  				  if(ir_received_char == keyboard[i][j].character){
	  					  sym_idx=keyboard[i][j].idx;
	  				  }
	  			  }
	  		  }
	  		  ir_data_ready = 0;

	  		  /* o tramite la logica ASCII
	  		   * if (ir_data_ready) {
					sym_idx = char_to_idx(ir_received_char);
					ir_data_ready = 0;
				}
	  		   */
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
  * @brief SPI1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI1_Init(void)
{

  /* USER CODE BEGIN SPI1_Init 0 */

  /* USER CODE END SPI1_Init 0 */

  /* USER CODE BEGIN SPI1_Init 1 */

  /* USER CODE END SPI1_Init 1 */
  /* SPI1 parameter configuration*/
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_4;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI1_Init 2 */

  /* USER CODE END SPI1_Init 2 */

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
  * @brief TIM5 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM5_Init(void)
{

  /* USER CODE BEGIN TIM5_Init 0 */

  /* USER CODE END TIM5_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM5_Init 1 */

  /* USER CODE END TIM5_Init 1 */
  htim5.Instance = TIM5;
  htim5.Init.Prescaler = 99;
  htim5.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim5.Init.Period = 3359;
  htim5.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim5.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim5) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim5, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim5, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM5_Init 2 */

  /* USER CODE END TIM5_Init 2 */

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
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA2_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA2_Stream3_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream3_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream3_IRQn);

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
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_8|GPIO_PIN_9|GPIO_PIN_10|GPIO_PIN_11, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_RESET);

  /*Configure GPIO pins : PC13 PC2 PC3 PC12 */
  GPIO_InitStruct.Pin = GPIO_PIN_13|GPIO_PIN_2|GPIO_PIN_3|GPIO_PIN_12;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pins : PC8 PC9 PC10 PC11 */
  GPIO_InitStruct.Pin = GPIO_PIN_8|GPIO_PIN_9|GPIO_PIN_10|GPIO_PIN_11;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pin : PB6 */
  GPIO_InitStruct.Pin = GPIO_PIN_6;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

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
