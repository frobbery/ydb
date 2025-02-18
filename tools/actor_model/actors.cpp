#include "actors.h"

#include "events.h"

#include <library/cpp/actors/core/actor_bootstrapped.h>

#include <library/cpp/actors/core/hfunc.h>

static auto ShouldContinue = std::make_shared < TProgramShouldContinue > ();

/*
Вам нужно написать реализацию TReadActor, TMaximumPrimeDevisorActor, TWriteActor
*/

/*
Требования к TReadActor:
1. Рекомендуется отнаследовать этот актор от NActors::TActorBootstrapped
2. В Boostrap этот актор отправляет себе NActors::TEvents::TEvWakeup
3. После получения этого сообщения считывается новое int64_t значение из strm
4. После этого порождается новый TMaximumPrimeDevisorActor который занимается вычислениями
5. Далее актор посылает себе сообщение NActors::TEvents::TEvWakeup чтобы не блокировать поток этим актором
6. Актор дожидается завершения всех TMaximumPrimeDevisorActor через TEvents::TEvDone
7. Когда чтение из файла завершено и получены подтверждения от всех TMaximumPrimeDevisorActor,
этот актор отправляет сообщение NActors::TEvents::TEvPoisonPill в TWriteActor
TReadActor
    Bootstrap:
        send(self, NActors::TEvents::TEvWakeup)

    NActors::TEvents::TEvWakeup:
        if read(strm) -> value:
            register(TMaximumPrimeDevisorActor(value, self, receipment))
            send(self, NActors::TEvents::TEvWakeup)
        else:
            ...

    TEvents::TEvDone:
        if Finish:
            send(receipment, NActors::TEvents::TEvPoisonPill)
        else:
            ...
*/

class TReadActor: public NActors::TActorBootstrapped < TReadActor > {
  const NActors::TActorId WriteActor;
  bool Finish = false;
  int counter;
  bool first_entered;

  public: TReadActor(const NActors::TActorId writeActor): WriteActor(writeActor),
  Finish(false),
  counter(0),
  first_entered(true) {}

  void Bootstrap() {
    Become( & TReadActor::StateFunc);
    Send(SelfId(), std::make_unique < NActors::TEvents::TEvWakeup > ());
  }

  STRICT_STFUNC(StateFunc, {
    cFunc(NActors::TEvents::TEvWakeup::EventType, HandleWakeUp);
    cFunc(TEvents::TEvDone::EventType, HandleDone);
  });

  void HandleWakeUp() {
    int64_t value;
    if (std::cin >> value) {
      first_entered = false;
      Register(CreateTMaximumPrimeDevisorActor(value, SelfId(), WriteActor).Release());
      counter++;
      Send(SelfId(), std::make_unique < NActors::TEvents::TEvWakeup > ());
    } else {
      if (first_entered) {
        Register(CreateTMaximumPrimeDevisorActor(0, SelfId(), WriteActor).Release());
        counter++;
        Send(SelfId(), std::make_unique < NActors::TEvents::TEvWakeup > ());
      }
      Finish = true;
    }
  }

  void HandleDone() {
    counter--;
    if (Finish && counter == 0) {
      Send(WriteActor, std::make_unique < NActors::TEvents::TEvPoisonPill > ());
    }
  }
};
/*
Требования к TMaximumPrimeDevisorActor:
1. Рекомендуется отнаследовать этот актор от NActors::TActorBootstrapped
2. В конструкторе этот актор принимает:
 - значение для которого нужно вычислить простое число
 - ActorId отправителя (ReadActor)
 - ActorId получателя (WriteActor)
2. В Boostrap этот актор отправляет себе NActors::TEvents::TEvWakeup по вызову которого происходит вызов Handler для вычислений
3. Вычисления нельзя проводить больше 10 миллисекунд
4. По истечении этого времени нужно сохранить текущее состояние вычислений в акторе и отправить себе NActors::TEvents::TEvWakeup
5. Когда результат вычислен он посылается в TWriteActor c использованием сообщения TEvWriteValueRequest
6. Далее отправляет ReadActor сообщение TEvents::TEvDone
7. Завершает свою работу

TMaximumPrimeDevisorActor
    Bootstrap:
        send(self, NActors::TEvents::TEvWakeup)

    NActors::TEvents::TEvWakeup:
        calculate
        if > 10 ms:
            Send(SelfId(), NActors::TEvents::TEvWakeup)
        else:
            Send(WriteActor, TEvents::TEvWriteValueRequest)
            Send(ReadActor, TEvents::TEvDone)
            PassAway()
*/

class TMaximumPrimeDevisorActor: public NActors::TActorBootstrapped < TMaximumPrimeDevisorActor > {
  const NActors::TActorIdentity ReadActor;
  const NActors::TActorId WriteActor;
  int64_t val;
  int64_t prime;
  int64_t current_first;
  int64_t current_second;
  bool flag;

  public: TMaximumPrimeDevisorActor(int64_t value,
    const NActors::TActorIdentity readActor,
      const NActors::TActorId writeActor): ReadActor(readActor),
  WriteActor(writeActor),
  val(value),
  prime(0),
  current_first(1),
  current_second(2),
  flag(true); {}

  void Bootstrap() {
    Become( & TMaximumPrimeDevisorActor::StateFunc);
    Send(SelfId(), std::make_unique < NActors::TEvents::TEvWakeup > ());
  }

  STRICT_STFUNC(StateFunc, {
    cFunc(NActors::TEvents::TEvWakeup::EventType, HandleWakeUp);
  });

  void HandleWakeUp() {
    auto startTime = std::chrono::steady_clock::now();
    auto endTime = std::chrono::steady_clock::now();
    auto elapsedTime = std::chrono::duration_cast < std::chrono::milliseconds > (endTime - startTime).count();
    for (int64_t i = current_first; i <= val; i++) {
      for (int64_t j = current_second; j * j <= i; j++) {
        if (i % j == 0) {
          flag = false;
          break;
        }
        endTime = std::chrono::steady_clock::now();
        elapsedTime = std::chrono::duration_cast < std::chrono::milliseconds > (endTime - startTime).count();
        if (elapsedTime > 10) {
          current_first = i;
          current_second = j;
          Send(SelfId(), std::make_unique < NActors::TEvents::TEvWakeup > ());
          return;
        }
      }
      if (flag && val % i == 0) {
        prime = i;
      } else {
        flag = true;
      }
    }
    Send(WriteActor, std::make_unique < TEvents::TEvWriteValueRequest > (prime));
    Send(ReadActor, std::make_unique < TEvents::TEvDone > ());
    PassAway();
  }
};
/*
Требования к TWriteActor:
1. Рекомендуется отнаследовать этот актор от NActors::TActor
2. Этот актор получает два типа сообщений NActors::TEvents::TEvPoisonPill::EventType и TEvents::TEvWriteValueRequest
2. В случае TEvents::TEvWriteValueRequest он принимает результат посчитанный в TMaximumPrimeDevisorActor и прибавляет его к локальной сумме
4. В случае NActors::TEvents::TEvPoisonPill::EventType актор выводит в Cout посчитанную локальнкую сумму, проставляет ShouldStop и завершает свое выполнение через PassAway

TWriteActor
    TEvents::TEvWriteValueRequest ev:
        Sum += ev->Value

    NActors::TEvents::TEvPoisonPill::EventType:
        Cout << Sum << Endl;
        ShouldStop()
        PassAway()
*/

class TWriteActor: public NActors::TActor < TWriteActor > {
  int Sum;
  public: using TBase = NActors::TActor < TWriteActor > ;

  TWriteActor(): TBase( & TWriteActor::Handler),
  Sum(0) {}

  STRICT_STFUNC(Handler, {
    hFunc(TEvents::TEvWriteValueRequest, Handle);
    cFunc(NActors::TEvents::TEvPoisonPill::EventType, HandleDone);
  });

  void Handle(TEvents::TEvWriteValueRequest::TPtr & ev) {
    auto & event = * ev -> Get();
    Sum = Sum + event.Value;
  }

  void HandleDone() {
    std::cout << Sum << std::endl;
    ShouldContinue -> ShouldStop();
    PassAway();
  }
};

class TSelfPingActor: public NActors::TActorBootstrapped < TSelfPingActor > {
  TDuration Latency;
  TInstant LastTime;

  public: TSelfPingActor(const TDuration & latency): Latency(latency) {}

  void Bootstrap() {
    LastTime = TInstant::Now();
    Become( & TSelfPingActor::StateFunc);
    Send(SelfId(), std::make_unique < NActors::TEvents::TEvWakeup > ());
  }

  STRICT_STFUNC(StateFunc, {
    cFunc(NActors::TEvents::TEvWakeup::EventType, HandleWakeup);
  });

  void HandleWakeup() {
    auto now = TInstant::Now();
    TDuration delta = now - LastTime;
    Y_VERIFY(delta <= Latency, "Latency too big");
    LastTime = now;
    Send(SelfId(), std::make_unique < NActors::TEvents::TEvWakeup > ());
  }
};

THolder < NActors::IActor > CreateSelfPingActor(const TDuration & latency) {
  return MakeHolder < TSelfPingActor > (latency);
}

std::shared_ptr < TProgramShouldContinue > GetProgramShouldContinue() {
  return ShouldContinue;
}
