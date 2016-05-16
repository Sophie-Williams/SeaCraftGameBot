Собрать программу:

    g++ -std=c++11 bot_main.cpp -o bot_main
  
Запуск:

    bot_main port_to_connect generator_seed(optional) time_interval_between_bot's_actions(in microseconds, optional)
    
Например:

    ./bot_main 1234 838 1000000
    
Перед таким запуском бота нужно сначала запустить сервер из репозитория https://github.com/alexgavrikov/SeaCraftGame командой:

    ./SeaCraftGame 1234

Бот в консоли ведёт рассказ о своих действиях.
В конце вывода бота-победителя может быть сообщение вида:

    My step is 5:8 and I I won.
    
Это не баг, просто сообщения от сервера могут приходить с опозданием, так как сервер шлёт сообщения только как ответы на запросы
бота. Сообщение о победе может слаться как ответ на запрос о следующем после победного ходе. 
