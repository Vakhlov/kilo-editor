/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

// Макрос применяет операцию побитового "И" к переданному символу и меняет старшие 3 бита на 0. Это отражает то, что
// делает `Ctrl` в терминале - обнуляет старшие 2 (два) бита. 5 бит (нумерация с нуля) в наборе симоволов ASCII отвечает
// за регистр: установкой и снятием бита осуществляется переключение между нижним и верхним регистром.
#define CTRL_KEY(k) ((k) & 0x1f)

#define KILO_VERSION "0.0.1"

enum editorKey {
	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	DEL_KEY,
	HOME_KEY,
	END_KEY,
	PAGE_UP,
	PAGE_DOWN
};

/*** data ***/

struct editorConfig {
	int cx;
	int cy;
	int screencols;
	int screenrows;
	struct termios originalTermios;
};

struct editorConfig config;

/*** terminal ***/

// Обработчик ошибок. `tcsetattr`, `tcgetattr` и `read` возвращают `-1` в случае неудачи и устанавливают глобальную
// переменную `errno`. `perror` использует `errno` и дополнительно выводит переданную строку.
// Чтобы намеренно вызвать ошибку в `tcgetattr`, нужно в командной строке передать файл или ввод через `|`:
// - `./kilo < kilo.c`
// - `echo test | ./kilo`
// Обе команды выведут: tcgetattr: Inappropriate ioctl for device
void die(const char *s) {
	// очистка экрана
	// см. комментарий в editorRefreshScreen
	// если бы мы сделали очистку в обработчике, переданном в `atexit`, мы бы не увидели, что напечатает `die`
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);

	perror(s);
	exit(1);
}

// Восстанавливает `canonical`-режим терминала
void disableRawMode() {
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &config.originalTermios) == -1) {
		die("tcsetattr");
	}
}

// Терминал может работать в `canonical`- или `cooked`-режиме или в `raw`-режиме. По умолчанию используется
// `canonical`-режим. В этом режиме ввод с клавиатуры направляется в программу только после нажатия клавиши `Enter`.
// В программах со сложным пользовательским интерфейсом, например, в текстовых редакторах, необходимо обрабатывать
// каждое нажатие клавиши и тут же выводить в интерфейс то, что набрано с клавиатуры. Это можно сделать в `raw`-режиме,
// однако, какого либо простого переключателя в этот режим нет. Нужно установить несколько флагов в терминале.
void enableRawMode() {
	// считываем атрибуты терминала
	if (tcgetattr(STDIN_FILENO, &config.originalTermios) == -1) {
		die("tcgetattr");
	}

	// установка обработчика выхода (восстанавливаем режим терминала)
	atexit(disableRawMode);

	struct termios raw = config.originalTermios;

	// меняем атрибуты
	// Флаг `BRKINT` выключается традиционно (скорее всего он уже выключен, трационность в его явном выключении). Если он
	// включен, условие прерывания спровоцирует отправку `SIGINT` процессу.
	// Флаг `INPCK` отвечает за проверку паритета, что, кажется, не актуально для современных эмуляторов терминалов.
	// Флаг `ISTRIP` отвечает за обнуление 8 бита каждого введенного байта.
	// Флаг `ICRNL` позволяет отключить автоматическое преобразование перевода каретки (`\r`) в новую строку (`\n`). Для
	// перевода каретки используется сочетание клавиш `Ctrl+M`, поэтому использование этого флага исправляет поведение
	// `Ctrl+M`. Однако, оно также поменяет код, возвращаемый клавишей `Enter`. Если флаг не отключить, и `Ctrl+M`, и
	// `Enter` вернут 10. Если отключить, , и `Ctrl+M`, и `Enter` вернут 13.
	// Флаг `IXON` позволяет отключить сочетания клавиш `Ctrl+S` и `Ctrl+Q`, которые управляют передачей данных терминалу.
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	// Битовая маска `CS8`. Устанавливает размер символа (CS) в 8 бит на байт.
	raw.c_cflag |= ~(CS8);
	// Флаг `ECHO` отвечает за вывод на экран символов, которые соответствуют нажимаемым клавишам. Это полезно в
	// `canonical`-режиме, но будет мешать при выводе интерфейса.
	// `c_lflag` - `local flags`. В комментариях в `termios.h` для `macOs` это поле описывается как свалка для разных
	// флагов. Кроме `c_lflags` есть еще `c_iflag` (флаги ввода), `c_oflag` (флаги вывода) и `c_cflag` (управляющие
	// (`control`) флаги).
	// `ECHO` - битовый флаг. Инвертируем его операцией побитового отрицания (длина - 32 бита). После побитового
	// отрицания, применяем операцию побитового "И" для установки значения флага в поле флагов.
	// Флаг `ICANON` позволяет выключить `canonical`-режим и считывать ввод байт за байтом вместо строки за строкой.
	// Флаг `IEXTEN` позволяет выключить использования сочетания клавиш `Ctrl+V` для расширенного ввода.
	// Флаг `ISIG` позволяет выключить отправку сигналов `SIGINT` и `SIGTSTP` процессу. Эти сигналы отправляются по
	// сочетанию клавиш `Ctrl+C` и `Ctrl+Z`.
	raw.c_lflag &= ~(ECHO | IEXTEN | ICANON | ISIG);
	// Флаг `OPOST` позволяет выключить преобразование `\n` в `\r\n` при выводе. Это единственная опция обработки вывода,
	// включенная по умолчанию. Если выключить преобразование, `printf` будет переводить курсор на новую строку, но не
	// будет возвращать его в начало строки.
	raw.c_oflag &= ~(OPOST);

	// Поле `cc` - `control characters`, управляющие символы - является массивом байтов, которые отвечают за различные
	// настройки терминала.
	// `VMIN` устанавливает минимальное число байт ввода чтобы вызов `read` что-то вернул. Установка `0` заставляет `read`
	// возвращать любой ввод сразу же.
	// `VTIME` - максимальное время ожидания перед тем как `read` вернет результат. Значение измеряется в десятых долях
	// секунды, поэтому `1` - это 100 миллисекунд. По прошествии этого времени `read` вернет `0` (это имеет смысл, т.к.
	// обычно `read` возвращает число прочитанных байт).
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;

	// устанавливаем атрибуты
	// `TCSAFLUSH` определяет, когда применить изменения. В данном случае, программа ждет записи в терминад всех
	// выводов, находящися в процессе ожидания и отбрасывает любой ввод, который еще не был прочитан.
	// `TCSAFLUSH` используется также в `disableRawMode`. Пожтому остаток ввода не отдается терминалу после выхода из
	// программы. Если этого не делать, то при вводе строки `123q456` программа последовательно будет считывать символы,
	// дойдя до `q` осуществит выход, а `456` будет воспринята как команда терминала. При использовании флага `ICANON`
	// работа программы изменится, т.к. будет работать считываение ввода байт за байтом. Поэтому, при нажатии `q` будет
	// осуществляться выход.
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
		die("tcsetattr");
	}
}

// Задача этой функции - ждать однократного нажатия клавиши и как только, вернуть введенный символ.
int editorReadKey() {
	int nread;
	char c;

	// в `Cygwin` по истечении таймаута `read` возвращает -1 и устанавливает `errno` в `EAGAIN` вместо того, чтобы
	// возвращать `0`. Поэтому в `Cygwin` не считаем `EAGAIN` за ошибку.

	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		if (nread == -1 && errno != EAGAIN) {
			die("read");
		}
	}

	// нажатие клавиш управления курсором (стрелки) приводит к считыванию `escape`-последовательности.
	if (c == '\x1b') {
		// если мы считали `escape`-символ, считываем за ним сразу еще 2 байта. Один из них `[`, второй - команда
		char seq[3];

		// считали первый байт после `escape`-символа (`[`)
		if (read(STDIN_FILENO, &seq[0], 1) != 1) {
			return '\x1b';
		}

		// считали второй байт после `escape`-символа (символ команды)
		if (read(STDIN_FILENO, &seq[1], 1) != 1) {
			return '\x1b';
		}

		// если это действительно `escape`-последовательность, а не просто нажатая клавиша `esc`, возвращаем
		// вместо команды нужный символ, который потом будет обработан в `editorProcessKeypress`
		if (seq[0] == '[') {
			// клавиши `page up` и `page down` посылают последовательности `<esc>[5~` и `<esc>[6~`
			// клавиши `home` и `end` посылают, в зависимости от ОС, последовательности `<esc>[1`, `<esc>[7`, `<esc>[H` или
			// `<esc>OH` и `<esc>[4`, `<esc>[8`, `<esc>[F` или `<esc>OF`
			// клавиша `delete` посылает вопледовательность `<esc>[3~`
			if (seq[1] >= '0' && seq[1] <= '9') {
				if (read(STDIN_FILENO, &seq[2], 1) != 1) {
					return '\x1b';
				}

				if (seq[2] == '~') {
					switch (seq[1]) {
						case '1':
							return HOME_KEY;
						case '3':
							return DEL_KEY;
						case '4':
							return END_KEY;
						case '5':
							return PAGE_UP;
						case '6':
							return PAGE_DOWN;
						case '7':
							return HOME_KEY;
						case '8':
							return END_KEY;
					}
				}
			} else {
				switch (seq[1]) {
					case 'A':
						return ARROW_UP;
					case 'B':
						return ARROW_DOWN;
					case 'C':
						return ARROW_RIGHT;
					case 'D':
						return ARROW_LEFT;
					case 'F':
						return END_KEY;
					case 'H':
						return HOME_KEY;
				}
			}
		} else if (seq[0] == 'O') {
			switch (seq[1]) {
				case 'F':
					return END_KEY;
				case 'H':
					return HOME_KEY;
			}
		}

		// если это не `escape`-последовательность (первый символ не `[`) или не обрабатываемая нами
		// `escape`-последовательность (команда), просто возвращаем считанный `escape`-код
		// вероятно, тут можно было сократить: обойтись без `else` и возвращать `c`
		return '\x1b';
	} else {
		return c;
	}
}

// получает положение курсора
int getCursorPosition(int *rows, int *cols) {
	char buf[32];
	unsigned int i = 0;

	// для запроса информации о статусе терминала можно использовать команду `n` (Device Status Report). Значение
	// аргумента `6` соответствует информации о курсоре. В ответ мы получим `escape`-последовательность `\x1b[71;271R`.
	// В документации это называется `Cursor Position Report`.
	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
		return -1;
	}

	while (i < sizeof(buf) - 1) {
		if (read(STDIN_FILENO, &buf[i], 1) != 1) {
			break;
		}

		if (buf[i] == 'R') {
			break;
		}

		i++;
	}

	// printf ожидает, что строка закончится символом `\0`, поэтому мы его добавляем
	buf[i] = '\0';
	// этот момент не совсем ясен: почему выводятся все символы начиная с первого, а не только первый
	//printf("\r\n&buf[1]: '%s'\r\n", &buf[1]);

	if (buf[0] != '\x1b' || buf[1] != '[') {
		return -1;
	}

	if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) {
		return -1;
	}

	return 0;
}

// Получает размер терминала в строках и столбцах. `TIOCGWINSZ` - это, вероятно, Terminal Input/Output Control Get
// WINdow SiZe.
int getWindowSize(int *rows, int *cols) {
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		// нет гарантии, что `ioctl` сможет получить размеры окна на всех системах
		// поэтому создаем обходное решение: поместим курсор к правый нижний угол и считаем его положение
		// команда `C` (Cursor Forward) перемещает курсор вправо
		// команда `B` (Cursor Down) перемещает курсор вниз
		// можно было бы использовать команду `H`, но в документации не сказано, что будет, если координаты больше, чем
		// ширина и высота окна.
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
			return -1;
		}

		return getCursorPosition(rows, cols);
	} else {
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

/*** append buffer ***/

struct abuf {
	char *b;
	int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
	// получаем блок памяти, который будет вмещать новую строку
	// `realloc` либо расширяет используемый блок памяти, либо освобождает память от него и предоставляет новый участок
	char *new = realloc(ab->b, ab->len + len);

	if (new == NULL) {
		return;
	}

	// копируем строку `s` в конец буфера
	memcpy(&new[ab->len], s, len);
	// обновляем указатель и длину в буфере
	ab->b = new;
	ab->len += len;
}

void abFree(struct abuf *ab) {
	free(ab->b);
}

/*** output ***/

// Пользовательский интерфейс будет перерисовываться с каждым нажатим какой-либо клавиши.

// выводит тильды по левому краю, как в `vim`
// функция будет обрабатывать каждую строку редактируемого текстового буфера
// с тильды начинаются все строки, не являющиеся частью файла. они не могут содержать текст.
void editorDrawRows(struct abuf *ab) {
	int y;

	for (y = 0; y < config.screenrows; y++) {
		// write(STDOUT_FILENO, "~", 1);
		if (y == config.screenrows / 3) {
			char welcome[80];
			int welcomeLen = snprintf(welcome, sizeof(welcome), "Kilo editor -- version %s", KILO_VERSION);

			// подстраховка для узких окон
			if (welcomeLen > config.screencols) {
				welcomeLen = config.screencols;
			}

			// центровка приветствия
			int padding = (config.screencols - welcomeLen) / 2;

			if (padding) {
				abAppend(ab, "~", 1);
				padding--;
			}

			while (padding--) {
				abAppend(ab, " ", 1);
			}
			// \центровка приветствия

			// вывод приветствия
			abAppend(ab, welcome, welcomeLen);
		} else {
			abAppend(ab, "~", 1);
		}

		// очистка строки до конца вместо очистки всего экрана в `editorRefreshScreen`
		// команда `K` (Erase In Line) очищает строку. Ее аргументы такие же как и у команды `J`, значение по умолчанию - 0
		abAppend(ab, "\x1b[K", 3);

		if (y < config.screenrows - 1) {
			// write(STDOUT_FILENO, "\r\n", 2);
			abAppend(ab, "\r\n", 2);
		}
	}
}

// обновляет экран
void editorRefreshScreen() {
	struct abuf ab = ABUF_INIT;


	// команды `h` (Set Mode) и `l` (Reset Mode) используются для включения и выключения разных возможностей или
	// режимов терминала. Значение аргумента `?25` не документировано в руководстве по `VT100`, видимо оно появилось
	// в более поздних моделях. Неизвестные команды и аргументы игнорируются терминалом.
	abAppend(&ab, "\x1b[?25l", 6);

	// 4 означает, что мы выводим 4 байта в терминал.
	// первый байт - \x1b или 27 в десятичной системе счисления - это `escape` символ. Остальные три байта - это `[2J`.
	// мы записываем в терминал `escape`-последовательность. Она всегда начинается с `escape`-символа `\x1b`, за которым
	// должен следовать символ `[`. `escape`-последовательности заставляют терминал выполнять различные задачи
	// форматирования текста: окрашивание в цвет, передвижение курсора и очистка частей экрана.
	// Далее мы используем команду `J` (Erase In Screen). Команды в `escape`-последовательностях могут принимать
	// аргументы, которые записываются перед ними.
	// 2 - очищает весь экран,
	// 1 - очищает эркан от начала и до курсора,
	// 0 - очищает экран от курсора и до конца. Это значение по умолчанию аргумента.
	// Используются команды терминала `VT100`.
	// write(STDOUT_FILENO, "\x1b[2J", 4);
	//abAppend(&ab, "\x1b[2J", 4);

	// Курсор остается внизу, перемещаем его в верхий левый угол. Для этого используем команду `H`. Она принимает 2
	// аргумента: номер строки и номер колонки. Аргументы разделяются символом `;`. Поэтому, если экран 80 на 24, то
	// для перемещения в центр экрана нужна команда `\x1b[12;40H`. Значение по умолчанию для обоих аргументов - 1
	// (строки и столбцы нумеруются с 1, не с 0).
	// write(STDOUT_FILENO, "\x1b[H", 3);
	abAppend(&ab, "\x1b[H", 3);

	editorDrawRows(&ab);

	// write(STDOUT_FILENO, "\x1b[H", 3);
	//abAppend(&ab, "\x1b[H", 3);
	// передвигаем курсор в нужное положение
	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", config.cy + 1, config.cx + 1);
	abAppend(&ab, buf, strlen(buf));

	abAppend(&ab, "\x1b[?25h", 6);

	write(STDOUT_FILENO, ab.b, ab.len);
	abFree(&ab);
}

/*** input ***/

void editorMoveCursor(int key) {
	switch (key) {
		case ARROW_LEFT:
			if (config.cx != 0) {
				config.cx--;
			}
			break;
		case ARROW_RIGHT:
			if (config.cx != config.screencols - 1) {
				config.cx++;
			}
			break;
		case ARROW_UP:
			if (config.cy != 0) {
				config.cy--;
			}
			break;
		case ARROW_DOWN:
			if (config.cy != config.screenrows - 1) {
				config.cy++;
			}
			break;
	}
}

// Эта функция ждет введенного символа и обрабатывает его.
void editorProcessKeypress() {
	int c = editorReadKey();

	switch (c) {
		case CTRL_KEY('q'):
			// очистка экрана
			// см. комментарий в editorRefreshScreen
			// если бы мы сделали очистку в обработчике, переданном в `atexit`, мы бы не увидели, что напечатает `die`
			write(STDOUT_FILENO, "\x1b[2J", 4);
			write(STDOUT_FILENO, "\x1b[H", 3);
			exit(0);
			break;
		case HOME_KEY:
			config.cx = 0;
			break;
		case END_KEY:
			config.cx = config.screencols - 1;
			break;
		case PAGE_UP:
		case PAGE_DOWN:
			{
				int times = config.screenrows;
				while (times--) {
					editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
				}
			}
			break;
		case ARROW_UP:
		case ARROW_DOWN:
		case ARROW_LEFT:
		case ARROW_RIGHT:
			editorMoveCursor(c);
			break;
	}
}

/*** init ***/

// Инициализация редактора: узнаем размер терминала в строках и столбцах.
void initEditor() {
	config.cx = 0;
	config.cy = 0;

	if (getWindowSize(&config.screenrows, &config.screencols) == -1) {
		die("getWindowSize");
	}
}

int main() {
	enableRawMode();
	initEditor();

	while(1) {
		editorRefreshScreen();
		editorProcessKeypress();
	}

	return 0;
}
