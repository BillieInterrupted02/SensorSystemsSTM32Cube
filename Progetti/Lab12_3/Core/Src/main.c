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
typedef enum { KEY_IDLE, KEY_DEBOUNCE, KEY_SENT } KeyState;
typedef enum { TX_IDLE, TX_START, TX_DATA, TX_STOP } TxState;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
// --- Tastiera ---
#define N_ROWS       4
#define N_COLS       4
#define DEBOUNCE_MS  30
#define COL_PORT     GPIOC
#define ROW_PORT     GPIOC
// --- Matrice LED ---
#define NUM_COLS     5
#define BLANK        16          // indice del "carattere" spento in FONT
#define RCLK_PORT    GPIOB
#define RCLK_PIN     GPIO_PIN_6
// --- IR ---
#define IR_PULSE     1105        // CCR3 = (ARR+1)/2 -> duty 50% a 38 kHz
#define TXQ_SIZE     32          // coda di trasmissione (byte)
#define DEBUG_UART   0           // metti 1 se hai configurato USART2 (stampa su PuTTY)
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
// --- Tastiera (come Lab10) ---
volatile KeyState key_state[N_ROWS][N_COLS] = {{KEY_IDLE}};
volatile uint32_t press_start[N_ROWS][N_COLS] = {{0}};
volatile uint8_t  current_col = 0;
const uint16_t COL_PIN[N_COLS] = {GPIO_PIN_8, GPIO_PIN_9, GPIO_PIN_10, GPIO_PIN_11};
const uint16_t ROW_PIN[N_ROWS] = {GPIO_PIN_12, GPIO_PIN_13, GPIO_PIN_2, GPIO_PIN_3};
const char keymap[N_ROWS][N_COLS] = {
    {'0','1','2','3'},
    {'4','5','6','7'},
    {'8','9','A','B'},
    {'C','D','E','F'}
};

// --- Matrice LED (come Lab09): un byte "riga" per ognuna delle 5 colonne, bit6 = riga in alto ---
static const uint8_t COL_W[NUM_COLS] = {16, 8, 4, 2, 1};
static const uint8_t FONT[17][NUM_COLS] = {
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
static volatile uint8_t sym_idx = BLANK;   // cosa mostra la matrice
static uint8_t col_cnt = 0;                // colonna della matrice in refresh

// --- Trasmettitore IR ---
static volatile TxState tx_state = TX_IDLE;
static volatile uint8_t tx_byte = 0;
static volatile uint8_t tx_bit_idx = 0;
static volatile uint8_t txq[TXQ_SIZE];
static volatile uint8_t txq_head = 0;      // scrive il main
static volatile uint8_t txq_tail = 0;      // legge la ISR di TIM4

// --- Ricevitore IR ---
static uint8_t rx_byte;                    // buffer per HAL_UART_Receive_IT
static volatile uint8_t rx_char = 0;       // ultimo byte ricevuto
static volatile uint8_t rx_new  = 0;       // flag "c'e' un byte nuovo"
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_SPI1_Init(void);
static void MX_TIM2_Init(void);
static void MX_TIM3_Init(void);
static void MX_TIM4_Init(void);
static void MX_TIM5_Init(void);
static void MX_USART1_UART_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* ---------------- IR TX ---------------- */
// Il PWM a 38 kHz resta sempre acceso: per "LED acceso" CCR3 = IR_PULSE, per "LED spento" CCR3 = 0
#define IR_ON()   __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_3, IR_PULSE)
#define IR_OFF()  __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_3, 0)

// bit '0' -> portante (LED pulsa); bit '1' -> LED spento
static inline void ir_write_bit(uint8_t bit)
{
    if (bit) IR_OFF(); else IR_ON();
}

// Inizia un frame: start bit = portante accesa subito
static void ir_start_frame(uint8_t b)
{
    tx_byte    = b;
    tx_bit_idx = 0;
    tx_state   = TX_START;
    IR_ON();
}

// Chiamata dalla ISR di TIM4 ogni 1/2400 s: passa al bit successivo
static void ir_tx_tick(void)
{
    switch (tx_state)
    {
    case TX_START:                              // e' finito lo start bit -> bit 0 (LSB)
        tx_bit_idx = 0;
        ir_write_bit(tx_byte & 1);
        tx_state = TX_DATA;
        break;

    case TX_DATA:
        tx_bit_idx++;
        if (tx_bit_idx < 8) {
            ir_write_bit((tx_byte >> tx_bit_idx) & 1);
        } else {                                // finiti gli 8 bit -> stop bit (LED spento)
            IR_OFF();
            tx_state = TX_STOP;
        }
        break;

    case TX_STOP:                               // e' finito lo stop bit
        if (txq_head != txq_tail) {             // c'e' un altro byte in coda: parte subito
            uint8_t b = txq[txq_tail];
            txq_tail = (txq_tail + 1) % TXQ_SIZE;
            ir_start_frame(b);
        } else {                                // altrimenti spegni il timer dei bit
            HAL_TIM_Base_Stop_IT(&htim4);
            tx_state = TX_IDLE;
        }
        break;

    default:
        break;
    }
}

// Step 1: invia un byte (non bloccante, salvo coda piena). Da chiamare dal main, NON da una ISR.
void IR_SendByte(uint8_t b)
{
    while (((txq_head + 1) % TXQ_SIZE) == txq_tail) { }   // coda piena: aspetta

    __disable_irq();                                      // evita la corsa con la ISR di TIM4
    if (tx_state == TX_IDLE) {
        ir_start_frame(b);
        __HAL_TIM_SET_COUNTER(&htim4, 0);
        __HAL_TIM_CLEAR_FLAG(&htim4, TIM_FLAG_UPDATE);    // niente interrupt spurio al primo start
        HAL_TIM_Base_Start_IT(&htim4);
    } else {
        txq[txq_head] = b;
        txq_head = (txq_head + 1) % TXQ_SIZE;
    }
    __enable_irq();
}

// Step 2: invia una stringa (un byte dopo l'altro)
void IR_SendString(const char *s)
{
    while (*s) IR_SendByte((uint8_t)*s++);
}

/* ---------------- Matrice LED (SPI + DMA) ---------------- */
static inline void spi_send(uint8_t col, uint8_t row)
{
    static uint8_t buffer[2];
    buffer[0] = row;     // daisy chain: prima la riga...
    buffer[1] = col;     // ...poi la colonna
    HAL_SPI_Transmit_DMA(&hspi1, buffer, 2);
}

void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance == SPI1) {
        HAL_GPIO_WritePin(RCLK_PORT, RCLK_PIN, GPIO_PIN_SET);     // impulso su RCLK = latch
        HAL_GPIO_WritePin(RCLK_PORT, RCLK_PIN, GPIO_PIN_RESET);
    }
}

/* ---------------- Callback UNICA per tutti i timer ---------------- */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM3)                    // refresh matrice (4 ms)
    {
        spi_send(COL_W[col_cnt], FONT[sym_idx][col_cnt]);
        col_cnt = (col_cnt + 1) % NUM_COLS;
    }
    else if (htim->Instance == TIM4)               // tick del bit IR (1/2400 s)
    {
        ir_tx_tick();
    }
    else if (htim->Instance == TIM5)               // scansione tastiera (come Lab10)
    {
        for (int row = 0; row < N_ROWS; row++)
        {
            GPIO_PinState r = HAL_GPIO_ReadPin(ROW_PORT, ROW_PIN[row]);
            if (r == GPIO_PIN_RESET)               // riga bassa -> tasto premuto
            {
                if (key_state[row][current_col] == KEY_IDLE)
                {
                    key_state[row][current_col] = KEY_DEBOUNCE;
                    press_start[row][current_col] = HAL_GetTick();
                }
            }
            else
            {
                key_state[row][current_col] = KEY_IDLE;
            }
        }
        HAL_GPIO_WritePin(COL_PORT, COL_PIN[current_col], GPIO_PIN_RESET);   // colonna corrente OFF
        current_col = (current_col + 1) % N_COLS;
        HAL_GPIO_WritePin(COL_PORT, COL_PIN[current_col], GPIO_PIN_SET);     // prossima colonna ON
    }
}

/* ---------------- IR RX (USART1, 1 byte per volta in interrupt) ---------------- */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1) {
        rx_char = rx_byte;
        rx_new  = 1;
        HAL_UART_Receive_IT(&huart1, &rx_byte, 1);       // riavvia subito la ricezione
    }
}

// Rumore sul canale IR -> framing/noise error: la HAL puo' interrompere la ricezione, quindi la riavvio
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1) {
        HAL_UART_Receive_IT(&huart1, &rx_byte, 1);       // se e' ancora attiva ritorna solo HAL_BUSY
    }
}

/* ---------------- Utility ---------------- */
// '0'..'9' -> 0..9, 'A'..'F' -> 10..15, altro -> 0xFF (ignorato)
static uint8_t char_to_glyph(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0xFF;
}

#if DEBUG_UART
static void debug_print(const char *tag, char c)
{
    char buf[16];
    int n = sprintf(buf, "%s %c\r\n", tag, c);
    HAL_UART_Transmit(&huart2, (uint8_t*)buf, n, 50);
}
#endif
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
  MX_SPI1_Init();
  MX_TIM2_Init();
  MX_TIM3_Init();
  MX_TIM4_Init();
  MX_TIM5_Init();
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */
  // Matrice
  HAL_TIM_Base_Start_IT(&htim3);

  // Tastiera: colonna 0 attiva (SET = transistor ON = colonna a GND), le altre spente
  HAL_GPIO_WritePin(COL_PORT, COL_PIN[0], GPIO_PIN_SET);
  for (int i = 1; i < N_COLS; i++) {
	  HAL_GPIO_WritePin(COL_PORT, COL_PIN[i], GPIO_PIN_RESET);
  }
  HAL_TIM_Base_Start_IT(&htim5);

  // IR: PWM a 38 kHz sempre acceso ma con CCR=0 (LED spento). TIM4 parte solo quando si trasmette
  IR_OFF();
  HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_3);
  HAL_UART_Receive_IT(&huart1, &rx_byte, 1);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
	// 1) tastiera -> IR
	for (int row = 0; row < N_ROWS; row++)
	{
		for (int col = 0; col < N_COLS; col++)
		{
			if (key_state[row][col] == KEY_DEBOUNCE &&
				(HAL_GetTick() - press_start[row][col]) >= DEBOUNCE_MS)
			{
				key_state[row][col] = KEY_SENT;              // non reinviare finche' e' premuto
				IR_SendByte((uint8_t)keymap[row][col]);
#if DEBUG_UART
				debug_print("TX", keymap[row][col]);
#endif
			}
		}
	}

	// 2) IR -> matrice LED
	if (rx_new)
	{
		uint8_t c = rx_char;
		rx_new = 0;
		uint8_t g = char_to_glyph(c);
		if (g != 0xFF) sym_idx = g;
#if DEBUG_UART
		debug_print("RX", (char)c);
#endif
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

  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 0;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 2210;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
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
  sConfigOC.Pulse = 1105;
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
  htim3.Init.Prescaler = 99;
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = 3359;
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
  htim4.Init.Prescaler = 99;
  htim4.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim4.Init.Period = 349;
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
  htim5.Init.Period = 4199;
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
