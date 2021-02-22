/*** includes ***/
// #define _DEFAULT_SOURCE;
// #define _BSD_SOURCE;
// #define _GNU_SOURCE;

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/
// Макрос применяет операцию побитового "И" к переданному символу и меняет старшие 3 бита на 0. Это отражает то, что
// делает `Ctrl` в терминале - обнуляет старшие 2 (два) бита. 5 бит (нумерация с нуля) в наборе симоволов ASCII отвечает
// за регистр: установкой и снятием бита осуществляется переключение между нижним и верхним регистром.
#define CTRL_KEY(k) ((k) & 0x1f)

// Константа с версией
#define KILO_VERSION "0.0.1"

// Константы для использования в функциях обработки ввода
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
// Тип для хранения строки текста
typedef struct editorRow {
	// Длина строки
	int size;
	// Символы в строке
	char *chars;
} editorRow;

// Конфигурация редактора, заполняется в процессе инициализации, изменяется в процессе работы. Отражает текущее
// состояние редактора.
struct editorConfig {
	// Положение курсора по горизонтали
	int cx;
	// Положение курсора по вертикали
	int cy;
	// Количество строк в окне терминала (высота окна терминала)
	int screencols;
	// Количество столбцов в окне терминала (ширина окна терминала)
	int screenrows;
	// Количество строк
	int numRows;
	// Строка текста
	editorRow *row;
	// Структура, хранящая настройки терминала
	struct termios originalTermios;
};

// Объявляем переменную для последующего использования
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
	// см. комментарий в `editorRefreshScreen`
	// если бы мы сделали очистку в обработчике, переданном в `atexit`, мы бы не увидели, что напечатает `die`
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);

	perror(s);
	exit(1);
}

// Восстанавливает `canonical`-режим терминала.
void disableRawMode() {
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &config.originalTermios) == -1) {
		die("tcsetattr");
	}
}

// Терминал может работать в `canonical`- или `cooked`-режиме или в `raw`-режиме. По умолчанию используется
// `canonical`-режим. В этом режиме ввод с клавиатуры направляется в программу только после нажатия клавиши `Enter`.
// В программах со сложным пользовательским интерфейсом, например, в текстовых редакторах, необходимо обрабатывать
// каждое нажатие клавиши и тут же выводить в интерфейс то, что набрано с клавиатуры. Это можно сделать в `raw`-режиме,
// однако, какого-то простого переключателя в этот режим нет. Нужно установить несколько флагов в настройках терминала.
void enableRawMode() {
	// считываем настройки терминала
	if (tcgetattr(STDIN_FILENO, &config.originalTermios) == -1) {
		die("tcgetattr");
	}

	// установка обработчика выхода (восстанавливаем режим терминала)
	atexit(disableRawMode);

	struct termios raw = config.originalTermios;

	// меняем атрибуты
	// в структуре `termios` есть несколько полей:
	// `c_lflag` - `local flags`. В комментариях в `termios.h` для `macOs` это поле описывается как свалка для разных
	// флагов.
	// `c_iflag` - `input flags` - флаги ввода,
	// `c_oflag` - `output flags` - флаги вывода,
	// `c_cflag` - `control flags` - управляющие флаги,
	// `cc` - `control characters`, управляющие символы - является массивом байтов, которые отвечают за различные
	// настройки терминала.

	// Флаг `BRKINT` выключается традиционно (скорее всего он уже выключен, трационность в его явном выключении). Если он
	// включен, условие прерывания спровоцирует отправку `SIGINT` процессу, что аналогично нажатию `Ctrl+C` для завершения
	// работы программы.
	// Флаг `INPCK` отвечает за проверку паритета, что, кажется, не актуально для современных эмуляторов терминалов.
	// Флаг `ISTRIP` отвечает за обнуление 8 бита каждого введенного байта.
	// Флаг `ICRNL` позволяет отключить автоматическое преобразование перевода каретки (`\r`) в новую строку (`\n`) при
	// вводе. Ожидается, что сочетание клавиш `Ctrl+M` вернет код 13, поскольку `M` - 13 буква алфавита, но вместо этого
	// оно возвращает код 10. Еще код 10 возвращают `Ctrl+J` и `Enter`. Терминал переводит возврат каретки (13, `\r`),
	// введенный пользователем в новую строку (10, `\n`). При отключении этого флага и `Ctrl+M`, и `Enter` будут
	// возвращать код 13. В названии флага `CR` - это `Carriage Return`, а `NL` - `New Line`.
	// Флаг `IXON` позволяет отключить сочетания клавиш `Ctrl+S` и `Ctrl+Q`, которые управляют передачей данных терминалу.
	// `Ctrl+S` останавливает передачу данных терминалу, `Ctrl+Q` - восстанавливает. Это может использоваться при работе,
	// например, с принтером, но в текстовом редакторе это не нужно.
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

	// Битовая маска `CS8`. Устанавливает размер символа (Character Size) в 8 бит на байт. Здесь мы именно устанавливаем
	// бит, а не сбрасываем, поэтому используется "побитовое ИЛИ"
	raw.c_cflag |= (CS8);

	// `ECHO` отвечает за вывод на экран символов, которые соответствуют нажимаемым клавишам. Это полезно в
	// `canonical`-режиме, но будет мешать при выводе интерфейса.
	// `ECHO` - битовое поле, его значение равно 0000 0000 0000 0000 0000 0000 0000 1000. Инвертируем его операцией
	// побитового отрицания. После этого, применяем операцию побитового "И" для установки значения флага в 0 в поле
	// флагов.
	// Флаг `ICANON` позволяет выключить `canonical`-режим и считывать ввод байт за байтом вместо строки за строкой.
	// Флаг `IEXTEN` позволяет выключить использования сочетания клавиш `Ctrl+V` для расширенного ввода. В некоторых
	// системах после ввода `Ctrl+V`, терминал ожидает, пока пользователь введет символ и затем пересылает этот символ
	// буквально. Например, до отключения `IEXTEN` можно было ввести `Ctrl+V`, а затем `Ctrl+C` для ввода трех байт.
	// Терминал отобразит это как `^C`, но это будут введенные символы, а не команда.
	// Флаг `ISIG` позволяет выключить отправку сигналов `SIGINT` и `SIGTSTP` процессу. Эти сигналы отправляются по
	// сочетанию клавиш `Ctrl+C` и `Ctrl+Z`. Первый используется для завершения работы процесса, второй - для останова
	// (suspend). Отключение этого флага также подействует на сочетание клавиш `Ctrl+Y` в `macOs`, которое работает так
	// же как и `Ctrl+Z`, но ждет завершения чтения программой ввода перед тем как остановить процесс.
	raw.c_lflag &= ~(ECHO | IEXTEN | ICANON | ISIG);

	// При выводе терминал преобразует символ новой строки `\n` в последовательность символов `\r\n`, т.е. добавляет
	// символ возврата каретки.
	// Флаг `OPOST` позволяет выключить преобразование `\n` в `\r\n` при выводе. Это единственная опция обработки вывода,
	// включенная по умолчанию. Если выключить преобразование, `printf` будет переводить курсор на новую строку, но не
	// будет возвращать его в начало строки.
	raw.c_oflag &= ~(OPOST);

	// `VMIN` устанавливает минимальное число байт ввода чтобы вызов `read` что-то вернул. Установка `0` заставляет `read`
	// возвращать любой ввод сразу же как он появится.
	// `VTIME` - максимальное время ожидания перед тем как `read` вернет результат. Значение измеряется в десятых долях
	// секунды, поэтому `1` - это 100 миллисекунд. По прошествии этого времени `read` вернет `0` (это имеет смысл, т.к.
	// обычно `read` возвращает число прочитанных байт).
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;

	// устанавливаем настройки
	// `TCSAFLUSH` определяет, когда применить изменения. В данном случае, программа ждет записи в терминал всех
	// выводов, находящися в процессе ожидания и отбрасывает любой ввод, который еще не был прочитан.
	// `TCSAFLUSH` используется также в `disableRawMode`. Поэтому остаток ввода не отдается терминалу после выхода из
	// программы. Если этого не делать, то при вводе строки `123q456` программа последовательно будет считывать символы,
	// дойдя до `q` осуществит выход (на момент написания комментария выход из программы осуществлялся по нажатию клавиши
	// `q`), а `456` будет воспринята как команда терминала (только в `Cygwin`). При использовании флага `ICANON` работа
	// программы изменится, т.к. будет работать считываение ввода байт за байтом. Поэтому, при нажатии `q` будет
	// осуществляться выход.
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
		die("tcsetattr");
	}
}

// Ждет однократного нажатия клавиши и как только клавиша будет нажата, возвращает введенный символ.
int editorReadKey() {
	int nread;
	char c;

	// в `Cygwin`, по истечении таймаута, `read` возвращает -1 и устанавливает `errno` в `EAGAIN` вместо того, чтобы
	// возвращать `0`. Поэтому в `Cygwin` не считаем `EAGAIN` за ошибку.
	// ждем ввода
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
			// клавиши `home` и `end` посылают, в зависимости от ОС, последовательности `<esc>[1~`, `<esc>[7~`, `<esc>[H` или
			// `<esc>OH` и `<esc>[4~`, `<esc>[8~`, `<esc>[F` или `<esc>OF`
			// клавиша `delete` посылает последовательность `<esc>[3~`
			// если третий байт в общей `escape`-последовательности соответствует клавишам 0 - 9, то, веротяно, это
			// последовательность `home`, `end`, `del`, `page up` или `page down`
			if (seq[1] >= '0' && seq[1] <= '9') {
				// считываем третий байт
				if (read(STDIN_FILENO, &seq[2], 1) != 1) {
					return '\x1b';
				}

				// если действительно последовательность заканчивается `~`, то это точно `home`, `end`, `del`, `page up`
				// или `page down`
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
				// третий байт в `escape`-последовательности не соответствует клавишам 0 - 9. Проверям, байт на соответствие
				// одной из обрабатываемых последовательностей: `A`, `B`, C` и `D` - это кнопки управления курсором (стрелки),
				// а `F` и `H` соответствуют альтернативным последовательностям для `end` и `home`.
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
			// некоторые `escape`-последовательности могут не содержать символа `[`. Вместо этого, они содержат `O`.
			// символы `F` и `H` соответствуют альтернативным последовательностям для `end` и `home`.
			switch (seq[1]) {
				case 'F':
					return END_KEY;
				case 'H':
					return HOME_KEY;
			}
		}

		// если это не `escape`-последовательность (первый символ не `[` и не `O`) или не обрабатываемая нами
		// `escape`-последовательность (команда), просто возвращаем считанный `escape`-код
		// вероятно, тут можно было сократить: обойтись без `else` и возвращать `c`
		return '\x1b';
	} else {
		return c;
	}
}

// Получает положение курсора.
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
	// поскольку указан `%s`, выводится строка из всех символов, начиная с первого, а не только первый символ
	// printf("\r\n&buf[1]: '%s'\r\n", &buf[1]);

	// если считали не `<escape>`-последовательность, это ошибка
	if (buf[0] != '\x1b' || buf[1] != '[') {
		return -1;
	}

	// считываем координаты курсора
	if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) {
		return -1;
	}

	return 0;
}

// Получает размер терминала в строках и столбцах. `TIOCGWINSZ` - это, вероятно, Terminal Input/Output Control Get
// WINdow SiZe.
// `winsize` из `sys.ioctl.h`.
int getWindowSize(int *rows, int *cols) {
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		// нет гарантии, что `ioctl` сможет получить размеры окна на всех системах.
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

/*** row operations ***/
// Выделяет память под новую строку, затем копирует переданную строку в структуру `editorRow` и добавляет в конец
// массива `config.row` в состоянии (там хранятся все строки, которые нужно вывести).
void editorAppendRow(char *s, size_t length) {
	// выделяем место под еще одну строку
	config.row = realloc(config.row, sizeof(editorRow) * (config.numRows + 1));

	// индекс добавляемой строки (равен количеству строк до добавления, т.к. индексация начинается с нуля)
	int at = config.numRows;

	// устанавливаем длину строки в состоянии
	config.row[at].size = length;
	// выделяем память и сохраняем ссылку в состоянии
	config.row[at].chars = malloc(length + 1);

	// копируем строку с состояние
	memcpy(config.row[at].chars, s, length);

	// добавляем символ конца строки
	config.row[at].chars[length] = '\0';
	// увеличиваем число строк
	config.numRows++;
}

/*** file i/o ***/
// Открывает и читает файлы с диска.
void editorOpen(char *filename) {
	// открываем файл
	FILE *fp = fopen(filename, "r");

	if (!fp) {
		die("fopen");
	}

	char *line = NULL;
	size_t lineCapacity = 0;
	ssize_t lineLength;

	// Считываем строки одну за одной. `getline` возвращает количество прочитанных символов. Эта функция полезна, когда
	// надо прочитать строку из файла и заранее не известна ее длина. Управление памятью функция берет на себя.
	// Сначала мы передаем в нее "нулевые" `line` и `lineCapacity`. Это заставляет ее выделить новый участок памяти для
	// строки, которую она прочитает. `line` будет указывать на выделенную память, а `lineCapacity` - количество
	// выделенной памяти. Функция вернет -1 если достигнет конца файла. При последующей передаче `line` и `lineCapactiy`
	// в `getline`, функция будет пытаться использовать память, на которую указывает `line` до тех пор, пока длина
	// строки не превысит `lineCapacity`.
	while ((lineLength = getline(&line, &lineCapacity, fp)) != -1) {
		// обрезаем `\n` и `\r` в конце строки
		while (lineLength > 0 && (line[lineLength - 1] == '\n' || line[lineLength - 1] == '\r')) {
			lineLength--;
		}

		// добавляем строку в массив строк для вывода
		editorAppendRow(line, lineLength);
	}

	// освобождаем память
	free(line);

	// закрываем файл
	fclose(fp);
}

/*** append buffer ***/
// В си нет динамических строк, поэтому делаем собственную реализацию с одной операцией - добавлением.

// Стуктура данных для накопителя.
struct abuf {
	// указатель на начало
	char *b;
	// текущая длина
	int len;
};

// Константа представляет пустой буфер. работает как своеобразный конструктор.
#define ABUF_INIT {NULL, 0}

// Увеличивает занимаемую память и добавляет строку в конец имеющейся в буфере.
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

// работает как своеобразный деструктор. освобождает память, занятую буфером.
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
		if (y >= config.numRows) {
			// выводим приветствие в первой трети экрана
			if (config.numRows == 0 && y == config.screenrows / 3) {
				// запись приветствия в буфер
				char welcome[80];
				int welcomeLen = snprintf(welcome, sizeof(welcome), "Kilo editor -- version %s", KILO_VERSION);

				// подстраховка для узких окон
				if (welcomeLen > config.screencols) {
					welcomeLen = config.screencols;
				}

				// центровка приветствия
				// считаем отступ слева (он равен отступу справа)
				int padding = (config.screencols - welcomeLen) / 2;

				if (padding) {
					// записываем тильду
					abAppend(ab, "~", 1);
					// уменьшаем отступ слева на единицу, т.к. вывели тильду
					padding--;
				}

				// заполняем отступ слева пробелами
				while (padding--) {
					abAppend(ab, " ", 1);
				}
				// \центровка приветствия

				// вывод приветствия
				abAppend(ab, welcome, welcomeLen);
			} else {
				// во всех остальных случаях просто выводим тильду
				abAppend(ab, "~", 1);
			}
		} else {
			int length = config.row[y].size;
			if (length > config.screencols) {
				length = config.screencols;
			}

			abAppend(ab, config.row[y].chars, length);
		}

		// очистка строки до конца вместо очистки всего экрана в `editorRefreshScreen`
		// команда `K` (Erase In Line) очищает строку. Ее аргументы такие же как и у команды `J`, значение по умолчанию - 0
		abAppend(ab, "\x1b[K", 3);

		// добавляем перевод строки в каждой строке
		if (y < config.screenrows - 1) {
			// write(STDOUT_FILENO, "\r\n", 2);
			abAppend(ab, "\r\n", 2);
		}
	}
}

// обновляет экран
void editorRefreshScreen() {
	// можно выводить интерфейс построчно, но лучше сначала записать весь интерфейс в буфер,
	// а потом вывести одной командой.

	// буфер для интерфейса
	struct abuf ab = ABUF_INIT;

	// прячем курсор
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
	// abAppend(&ab, "\x1b[2J", 4);

	// Курсор остается внизу, перемещаем его в верхий левый угол. Для этого используем команду `H`. Она принимает 2
	// аргумента: номер строки и номер колонки. Аргументы разделяются символом `;`. Поэтому, если экран 80 на 24, то
	// для перемещения в центр экрана нужна команда `\x1b[12;40H`. Значение по умолчанию для обоих аргументов - 1
	// (строки и столбцы нумеруются с 1, не с 0).
	// write(STDOUT_FILENO, "\x1b[H", 3);
	abAppend(&ab, "\x1b[H", 3);

	// выводим текстовый интерфейс в буфер
	editorDrawRows(&ab);

	// передвигаем курсор в нужное положение
	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", config.cy + 1, config.cx + 1);
	abAppend(&ab, buf, strlen(buf));

	// показываем курсор
	abAppend(&ab, "\x1b[?25h", 6);

	// выводим содержимое буфера
	write(STDOUT_FILENO, ab.b, ab.len);

	// освобождаем память
	abFree(&ab);
}

/*** input ***/
// Меняет координаты курсора в текущей конфигурации приложения. Фактически курсор перемещается при следующем
// выводе интерфейса.
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
	// считали символ. он может быть одиночным символом или началом `escape`-последовательности. Во втором случае вместо
	// последовательности возвращается специальная константа в зависимости от того, что за `escape`-последовательность
	// был введена.
	int c = editorReadKey();

	switch (c) {
		case CTRL_KEY('q'):
			// очистка экрана перед выходом (см. комментарий в editorRefreshScreen)
			// если бы мы сделали очистку в обработчике, переданном в `atexit`, мы бы не увидели, что напечатает `die`
			write(STDOUT_FILENO, "\x1b[2J", 4);
			// возврат курсора на место
			write(STDOUT_FILENO, "\x1b[H", 3);
			// выход
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
	// текущие координаты курсора
	config.cx = 0;
	config.cy = 0;

	//
	config.numRows = 0;

	config.row = NULL;

	// чтение размеров окна
	if (getWindowSize(&config.screenrows, &config.screencols) == -1) {
		die("getWindowSize");
	}
}

/*** --- ***/
int main(int argc, char *argv[]) {
	// включаем `raw`-режим
	enableRawMode();
	// инициализируем редактор
	initEditor();

	// если в качестве аргумента командной строки передан файл, открываем его
	if (argc >= 2) {
		editorOpen(argv[1]);
	}

	while(1) {
		// рисуем интерфейс
		editorRefreshScreen();
		// обрабатываем нажатие клавиш
		editorProcessKeypress();
	}

	return 0;
}
